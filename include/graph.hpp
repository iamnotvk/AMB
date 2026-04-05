#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace amb {

using VertexId = std::uint32_t;

struct CSRGraph {
    std::size_t vertex_count {0};
    std::size_t edge_count {0};
    std::vector<std::uint64_t> offsets;
    std::vector<VertexId> edges;
    std::vector<std::uint32_t> out_degree;

    [[nodiscard]] std::size_t degree(VertexId vertex) const;
    [[nodiscard]] std::pair<const VertexId*, const VertexId*> neighbors(VertexId vertex) const;
    [[nodiscard]] static CSRGraph from_edge_list_file(const std::string& path, bool make_undirected = false);
};

struct AdjacencyGraph {
    std::vector<std::vector<VertexId>> adjacency;

    [[nodiscard]] std::size_t vertex_count() const;
    [[nodiscard]] std::size_t edge_count() const;
    [[nodiscard]] static AdjacencyGraph from_csr(const CSRGraph& graph);
};

struct GraphStats {
    double load_seconds {0.0};
    std::size_t vertices {0};
    std::size_t edges {0};
};

struct TraversalEvent {
    std::size_t level {0};
    VertexId vertex {0};
};

struct AtomicDoubleArray {
    std::vector<std::atomic<std::uint64_t>> storage;

    explicit AtomicDoubleArray(std::size_t size);
    void store(std::size_t index, double value, std::memory_order order = std::memory_order_relaxed);
    double load(std::size_t index, std::memory_order order = std::memory_order_relaxed) const;
    double fetch_add(std::size_t index, double delta, std::memory_order order = std::memory_order_relaxed);
    void reset(double value, std::memory_order order = std::memory_order_relaxed);
};

struct MetricSnapshot {
    double elapsed_seconds {0.0};
    double teps {0.0};
    double atomic_seconds {0.0};
    double compute_seconds {0.0};
    double synchronization_ratio {0.0};
    double estimated_llc_miss_rate {0.0};
    double estimated_bandwidth_gbps {0.0};
};

struct PageRankResult {
    std::vector<double> scores;
    MetricSnapshot metrics;
};

struct BFSResult {
    std::vector<int> level;
    std::vector<TraversalEvent> trace;
    MetricSnapshot metrics;
};

struct BenchmarkEntry {
    std::string label;
    MetricSnapshot metrics;
    double speedup_vs_baseline {0.0};
    std::size_t trial_count {0};
    double min_elapsed_seconds {0.0};
    double max_elapsed_seconds {0.0};
    std::vector<double> elapsed_samples;
    std::vector<double> teps_samples;
};

struct BenchmarkReport {
    std::string algorithm;
    std::vector<BenchmarkEntry> entries;
};

PageRankResult run_pagerank(const CSRGraph& graph, std::size_t iterations, std::size_t thread_count, double damping);
BFSResult run_parallel_bfs(const CSRGraph& graph, VertexId source, std::size_t thread_count, bool capture_trace);
PageRankResult run_baseline_pagerank(const AdjacencyGraph& graph, std::size_t iterations, double damping);
BFSResult run_mutex_bfs(const AdjacencyGraph& graph, VertexId source, std::size_t thread_count);
BenchmarkReport benchmark_pagerank(const CSRGraph& csr_graph,
                                   const AdjacencyGraph& adjacency_graph,
                                   std::size_t iterations,
                                   std::size_t thread_count,
                                   double damping,
                                   std::size_t trials);
BenchmarkReport benchmark_bfs(const CSRGraph& csr_graph,
                              const AdjacencyGraph& adjacency_graph,
                              VertexId source,
                              std::size_t thread_count,
                              std::size_t trials);
std::string build_metrics_json(const MetricSnapshot& metrics);
std::string build_trace_json(const CSRGraph& graph, const BFSResult& bfs);
std::string build_benchmark_json(const BenchmarkReport& report);

}  // namespace amb
