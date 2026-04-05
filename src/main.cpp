#include "graph.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

struct Options {
    std::string graph_path;
    std::string algorithm {"pagerank"};
    std::string emit_trace_path;
    std::string emit_benchmark_path;
    std::size_t iterations {20};
    std::size_t thread_count {std::max(1u, std::thread::hardware_concurrency())};
    std::size_t benchmark_trials {5};
    double damping {0.85};
    amb::VertexId source {0};
    bool undirected {false};
    bool benchmark {false};
};

void print_usage() {
    std::cout
        << "Usage: build/amb_graph --graph <path> [--algorithm pagerank|bfs] [--threads N]\n"
        << "                       [--iterations N] [--damping X] [--source V] [--trials N]\n"
        << "                       [--emit-trace web/trace.json] [--emit-benchmark web/benchmark.json]\n"
        << "                       [--benchmark] [--undirected]\n";
}

Options parse_args(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--graph" && i + 1 < argc) {
            options.graph_path = argv[++i];
        } else if (arg == "--algorithm" && i + 1 < argc) {
            options.algorithm = argv[++i];
        } else if (arg == "--iterations" && i + 1 < argc) {
            options.iterations = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if (arg == "--threads" && i + 1 < argc) {
            options.thread_count = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if (arg == "--trials" && i + 1 < argc) {
            options.benchmark_trials = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if (arg == "--damping" && i + 1 < argc) {
            options.damping = std::stod(argv[++i]);
        } else if (arg == "--source" && i + 1 < argc) {
            options.source = static_cast<amb::VertexId>(std::stoul(argv[++i]));
        } else if (arg == "--emit-trace" && i + 1 < argc) {
            options.emit_trace_path = argv[++i];
        } else if (arg == "--emit-benchmark" && i + 1 < argc) {
            options.emit_benchmark_path = argv[++i];
        } else if (arg == "--undirected") {
            options.undirected = true;
        } else if (arg == "--benchmark") {
            options.benchmark = true;
        } else if (arg == "--help") {
            print_usage();
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }
    if (options.graph_path.empty()) {
        throw std::runtime_error("--graph is required");
    }
    return options;
}

void print_metrics(const amb::MetricSnapshot& metrics) {
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "Metrics\n";
    std::cout << "  TEPS: " << metrics.teps << "\n";
    std::cout << "  Elapsed: " << metrics.elapsed_seconds << " s\n";
    std::cout << "  Atomic time: " << metrics.atomic_seconds << " s\n";
    std::cout << "  Compute time: " << metrics.compute_seconds << " s\n";
    std::cout << "  Sync ratio: " << metrics.synchronization_ratio << "\n";
    std::cout << "  Estimated LLC miss rate: " << metrics.estimated_llc_miss_rate << "\n";
    std::cout << "  Estimated bandwidth: " << metrics.estimated_bandwidth_gbps << " GB/s\n";
}

void print_benchmark(const amb::BenchmarkReport& report) {
    std::cout << "Benchmark Comparison\n";
    for (const auto& entry : report.entries) {
        std::cout << "  " << entry.label << "\n";
        std::cout << "    TEPS: " << entry.metrics.teps << "\n";
        std::cout << "    Elapsed: " << entry.metrics.elapsed_seconds << " s\n";
        std::cout << "    Trials: " << entry.trial_count << "\n";
        std::cout << "    Min/Max elapsed: " << entry.min_elapsed_seconds << " / " << entry.max_elapsed_seconds << " s\n";
        std::cout << "    Speedup vs baseline: " << entry.speedup_vs_baseline << "x\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_args(argc, argv);
        const auto graph = amb::CSRGraph::from_edge_list_file(options.graph_path, options.undirected);
        const auto adjacency = amb::AdjacencyGraph::from_csr(graph);

        if (options.algorithm == "pagerank") {
            const auto result = amb::run_pagerank(graph, options.iterations, options.thread_count, options.damping);
            std::cout << "Top PageRank vertices\n";
            for (std::size_t vertex = 0; vertex < std::min<std::size_t>(10, result.scores.size()); ++vertex) {
                std::cout << "  v" << vertex << ": " << result.scores[vertex] << "\n";
            }
            print_metrics(result.metrics);
            if (options.benchmark || !options.emit_benchmark_path.empty()) {
                const auto report =
                    amb::benchmark_pagerank(graph, adjacency, options.iterations, options.thread_count, options.damping, options.benchmark_trials);
                print_benchmark(report);
                if (!options.emit_benchmark_path.empty()) {
                    std::ofstream output(options.emit_benchmark_path);
                    if (!output) {
                        throw std::runtime_error("Failed to write benchmark file: " + options.emit_benchmark_path);
                    }
                    output << amb::build_benchmark_json(report);
                    std::cout << "Wrote benchmark report to " << options.emit_benchmark_path << "\n";
                }
            }
        } else if (options.algorithm == "bfs") {
            const auto result = amb::run_parallel_bfs(graph, options.source, options.thread_count, !options.emit_trace_path.empty());
            std::cout << "BFS levels\n";
            for (std::size_t vertex = 0; vertex < std::min<std::size_t>(20, result.level.size()); ++vertex) {
                std::cout << "  v" << vertex << ": " << result.level[vertex] << "\n";
            }
            print_metrics(result.metrics);

            if (!options.emit_trace_path.empty()) {
                std::ofstream output(options.emit_trace_path);
                if (!output) {
                    throw std::runtime_error("Failed to write trace file: " + options.emit_trace_path);
                }
                output << amb::build_trace_json(graph, result);
                std::cout << "Wrote traversal trace to " << options.emit_trace_path << "\n";
            }
            if (options.benchmark || !options.emit_benchmark_path.empty()) {
                const auto report = amb::benchmark_bfs(graph, adjacency, options.source, options.thread_count, options.benchmark_trials);
                print_benchmark(report);
                if (!options.emit_benchmark_path.empty()) {
                    std::ofstream output(options.emit_benchmark_path);
                    if (!output) {
                        throw std::runtime_error("Failed to write benchmark file: " + options.emit_benchmark_path);
                    }
                    output << amb::build_benchmark_json(report);
                    std::cout << "Wrote benchmark report to " << options.emit_benchmark_path << "\n";
                }
            }
        } else {
            throw std::runtime_error("Unsupported algorithm: " + options.algorithm);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        print_usage();
        return 1;
    }

    return 0;
}
