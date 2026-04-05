#include "graph.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <thread>

namespace amb {

namespace {

struct ThreadStats {
    double atomic_seconds {0.0};
    double compute_seconds {0.0};
    std::size_t traversed_edges {0};
};

double seconds_since(const std::chrono::steady_clock::time_point& start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

MetricSnapshot finalize_metrics(double elapsed,
                                std::size_t traversed_edges,
                                double atomic_seconds,
                                double compute_seconds,
                                std::size_t thread_count) {
    MetricSnapshot metrics;
    metrics.elapsed_seconds = elapsed;
    metrics.teps = elapsed > 0.0 ? static_cast<double>(traversed_edges) / elapsed : 0.0;
    metrics.atomic_seconds = atomic_seconds;
    metrics.compute_seconds = compute_seconds;
    const double total = atomic_seconds + compute_seconds;
    metrics.synchronization_ratio = total > 0.0 ? atomic_seconds / total : 0.0;

    // Approximate contention-driven LLC behavior to make the demo measurable without PMU access.
    const double contention_factor = std::min(1.0, metrics.synchronization_ratio * 1.6);
    metrics.estimated_llc_miss_rate = std::min(0.95, 0.08 + contention_factor * 0.55 + 0.015 * thread_count);

    // Edge updates on CSR typically read source state, edge endpoint, and target state metadata.
    const double bytes_touched = static_cast<double>(traversed_edges) * 20.0;
    metrics.estimated_bandwidth_gbps = elapsed > 0.0 ? (bytes_touched / elapsed) / 1'000'000'000.0 : 0.0;
    return metrics;
}

std::vector<std::pair<std::size_t, std::size_t>> partition_range(std::size_t count, std::size_t parts) {
    parts = std::max<std::size_t>(1, parts);
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    ranges.reserve(parts);
    const std::size_t chunk = count / parts;
    const std::size_t remainder = count % parts;
    std::size_t cursor = 0;
    for (std::size_t i = 0; i < parts; ++i) {
        const std::size_t width = chunk + (i < remainder ? 1 : 0);
        ranges.emplace_back(cursor, cursor + width);
        cursor += width;
    }
    return ranges;
}

MetricSnapshot average_metrics(const std::vector<MetricSnapshot>& samples) {
    MetricSnapshot avg;
    if (samples.empty()) {
        return avg;
    }

    for (const auto& sample : samples) {
        avg.elapsed_seconds += sample.elapsed_seconds;
        avg.teps += sample.teps;
        avg.atomic_seconds += sample.atomic_seconds;
        avg.compute_seconds += sample.compute_seconds;
        avg.synchronization_ratio += sample.synchronization_ratio;
        avg.estimated_llc_miss_rate += sample.estimated_llc_miss_rate;
        avg.estimated_bandwidth_gbps += sample.estimated_bandwidth_gbps;
    }

    const double scale = 1.0 / static_cast<double>(samples.size());
    avg.elapsed_seconds *= scale;
    avg.teps *= scale;
    avg.atomic_seconds *= scale;
    avg.compute_seconds *= scale;
    avg.synchronization_ratio *= scale;
    avg.estimated_llc_miss_rate *= scale;
    avg.estimated_bandwidth_gbps *= scale;
    return avg;
}

BenchmarkEntry summarize_entry(std::string label, const std::vector<MetricSnapshot>& samples) {
    BenchmarkEntry entry;
    entry.label = std::move(label);
    entry.metrics = average_metrics(samples);
    entry.trial_count = samples.size();
    if (samples.empty()) {
        return entry;
    }

    entry.min_elapsed_seconds = samples.front().elapsed_seconds;
    entry.max_elapsed_seconds = samples.front().elapsed_seconds;
    entry.elapsed_samples.reserve(samples.size());
    entry.teps_samples.reserve(samples.size());
    for (const auto& sample : samples) {
        entry.min_elapsed_seconds = std::min(entry.min_elapsed_seconds, sample.elapsed_seconds);
        entry.max_elapsed_seconds = std::max(entry.max_elapsed_seconds, sample.elapsed_seconds);
        entry.elapsed_samples.push_back(sample.elapsed_seconds);
        entry.teps_samples.push_back(sample.teps);
    }
    return entry;
}

}  // namespace

PageRankResult run_pagerank(const CSRGraph& graph, std::size_t iterations, std::size_t thread_count, double damping) {
    thread_count = std::max<std::size_t>(1, thread_count);
    const std::size_t n = graph.vertex_count;
    std::vector<double> rank(n, n > 0 ? 1.0 / static_cast<double>(n) : 0.0);
    std::vector<double> next_rank(n, 0.0);
    AtomicDoubleArray contributions(n);
    const auto ranges = partition_range(n, thread_count);
    std::vector<ThreadStats> thread_stats(thread_count);
    const auto global_start = std::chrono::steady_clock::now();

    for (std::size_t iter = 0; iter < iterations; ++iter) {
        contributions.reset(0.0);
        std::vector<std::thread> workers;
        workers.reserve(thread_count);

        for (std::size_t worker = 0; worker < thread_count; ++worker) {
            const auto [begin, end] = ranges[worker];
            workers.emplace_back([&, worker, begin, end] {
                for (std::size_t vertex = begin; vertex < end; ++vertex) {
                    const auto degree = graph.out_degree[vertex];
                    if (degree == 0) {
                        continue;
                    }
                    const auto compute_start = std::chrono::steady_clock::now();
                    const double share = rank[vertex] / static_cast<double>(degree);
                    thread_stats[worker].compute_seconds += seconds_since(compute_start);

                    const auto [nbr_begin, nbr_end] = graph.neighbors(static_cast<VertexId>(vertex));
                    for (auto it = nbr_begin; it != nbr_end; ++it) {
                        const auto atomic_start = std::chrono::steady_clock::now();
                        contributions.fetch_add(*it, share, std::memory_order_relaxed);
                        thread_stats[worker].atomic_seconds += seconds_since(atomic_start);
                        thread_stats[worker].traversed_edges += 1;
                    }
                }
            });
        }

        for (auto& worker : workers) {
            worker.join();
        }

        const double base = n > 0 ? (1.0 - damping) / static_cast<double>(n) : 0.0;
        std::vector<std::thread> reduction_workers;
        reduction_workers.reserve(thread_count);
        for (std::size_t worker = 0; worker < thread_count; ++worker) {
            const auto [begin, end] = ranges[worker];
            reduction_workers.emplace_back([&, begin, end] {
                for (std::size_t vertex = begin; vertex < end; ++vertex) {
                    next_rank[vertex] = base + damping * contributions.load(vertex, std::memory_order_acquire);
                }
            });
        }
        for (auto& worker : reduction_workers) {
            worker.join();
        }
        rank.swap(next_rank);
    }

    const double elapsed = seconds_since(global_start);
    const double atomic_seconds = std::accumulate(thread_stats.begin(), thread_stats.end(), 0.0,
                                                  [](double acc, const ThreadStats& stats) {
                                                      return acc + stats.atomic_seconds;
                                                  });
    const double compute_seconds = std::accumulate(thread_stats.begin(), thread_stats.end(), 0.0,
                                                   [](double acc, const ThreadStats& stats) {
                                                       return acc + stats.compute_seconds;
                                                   });
    const std::size_t traversed_edges = std::accumulate(thread_stats.begin(), thread_stats.end(), std::size_t {0},
                                                        [](std::size_t acc, const ThreadStats& stats) {
                                                            return acc + stats.traversed_edges;
                                                        });

    return {rank, finalize_metrics(elapsed, traversed_edges, atomic_seconds, compute_seconds, thread_count)};
}

PageRankResult run_baseline_pagerank(const AdjacencyGraph& graph, std::size_t iterations, double damping) {
    const std::size_t n = graph.vertex_count();
    std::vector<double> rank(n, n > 0 ? 1.0 / static_cast<double>(n) : 0.0);
    std::vector<double> next_rank(n, 0.0);
    std::size_t traversed_edges = 0;
    double compute_seconds = 0.0;
    const auto global_start = std::chrono::steady_clock::now();

    for (std::size_t iter = 0; iter < iterations; ++iter) {
        std::fill(next_rank.begin(), next_rank.end(), 0.0);
        for (std::size_t vertex = 0; vertex < n; ++vertex) {
            const auto compute_start = std::chrono::steady_clock::now();
            const auto degree = graph.adjacency[vertex].size();
            if (degree > 0) {
                const double share = rank[vertex] / static_cast<double>(degree);
                for (const auto neighbor : graph.adjacency[vertex]) {
                    next_rank[neighbor] += share;
                    traversed_edges += 1;
                }
            }
            compute_seconds += seconds_since(compute_start);
        }

        const double base = n > 0 ? (1.0 - damping) / static_cast<double>(n) : 0.0;
        for (std::size_t vertex = 0; vertex < n; ++vertex) {
            next_rank[vertex] = base + damping * next_rank[vertex];
        }
        rank.swap(next_rank);
    }

    return {rank, finalize_metrics(seconds_since(global_start), traversed_edges, 0.0, compute_seconds, 1)};
}

BFSResult run_parallel_bfs(const CSRGraph& graph, VertexId source, std::size_t thread_count, bool capture_trace) {
    thread_count = std::max<std::size_t>(1, thread_count);
    const std::size_t n = graph.vertex_count;
    std::vector<int> level(n, -1);
    std::vector<TraversalEvent> trace;
    std::mutex trace_mutex;
    std::vector<ThreadStats> thread_stats(thread_count);

    if (source >= n) {
        return {level, trace, {}};
    }

    std::vector<VertexId> frontier {source};
    level[source] = 0;
    if (capture_trace) {
        trace.push_back({0, source});
    }

    std::vector<std::atomic<int>> visited(n);
    for (std::size_t i = 0; i < n; ++i) {
        visited[i].store(level[i], std::memory_order_relaxed);
    }

    std::size_t current_level = 0;
    const auto global_start = std::chrono::steady_clock::now();

    while (!frontier.empty()) {
        const auto ranges = partition_range(frontier.size(), thread_count);
        std::vector<std::vector<VertexId>> local_next(thread_count);
        std::vector<std::vector<TraversalEvent>> local_trace(thread_count);
        std::vector<std::thread> workers;
        workers.reserve(thread_count);

        for (std::size_t worker = 0; worker < thread_count; ++worker) {
            const auto [begin, end] = ranges[worker];
            workers.emplace_back([&, worker, begin, end] {
                for (std::size_t index = begin; index < end; ++index) {
                    const VertexId vertex = frontier[index];
                    const auto [nbr_begin, nbr_end] = graph.neighbors(vertex);
                    for (auto it = nbr_begin; it != nbr_end; ++it) {
                        const auto compute_start = std::chrono::steady_clock::now();
                        const VertexId neighbor = *it;
                        thread_stats[worker].compute_seconds += seconds_since(compute_start);

                        const auto atomic_start = std::chrono::steady_clock::now();
                        int expected = -1;
                        if (visited[neighbor].compare_exchange_strong(expected,
                                                                     static_cast<int>(current_level + 1),
                                                                     std::memory_order_acq_rel,
                                                                     std::memory_order_relaxed)) {
                            local_next[worker].push_back(neighbor);
                            if (capture_trace) {
                                local_trace[worker].push_back({current_level + 1, neighbor});
                            }
                        }
                        thread_stats[worker].atomic_seconds += seconds_since(atomic_start);
                        thread_stats[worker].traversed_edges += 1;
                    }
                }
            });
        }

        for (auto& worker : workers) {
            worker.join();
        }

        frontier.clear();
        for (std::size_t worker = 0; worker < thread_count; ++worker) {
            frontier.insert(frontier.end(), local_next[worker].begin(), local_next[worker].end());
            if (capture_trace) {
                trace.insert(trace.end(), local_trace[worker].begin(), local_trace[worker].end());
            }
        }
        ++current_level;
    }

    for (std::size_t vertex = 0; vertex < n; ++vertex) {
        level[vertex] = visited[vertex].load(std::memory_order_acquire);
    }

    const double elapsed = seconds_since(global_start);
    const double atomic_seconds = std::accumulate(thread_stats.begin(), thread_stats.end(), 0.0,
                                                  [](double acc, const ThreadStats& stats) {
                                                      return acc + stats.atomic_seconds;
                                                  });
    const double compute_seconds = std::accumulate(thread_stats.begin(), thread_stats.end(), 0.0,
                                                   [](double acc, const ThreadStats& stats) {
                                                       return acc + stats.compute_seconds;
                                                   });
    const std::size_t traversed_edges = std::accumulate(thread_stats.begin(), thread_stats.end(), std::size_t {0},
                                                        [](std::size_t acc, const ThreadStats& stats) {
                                                            return acc + stats.traversed_edges;
                                                        });

    return {level, trace, finalize_metrics(elapsed, traversed_edges, atomic_seconds, compute_seconds, thread_count)};
}

BFSResult run_mutex_bfs(const AdjacencyGraph& graph, VertexId source, std::size_t thread_count) {
    thread_count = std::max<std::size_t>(1, thread_count);
    const std::size_t n = graph.vertex_count();
    std::vector<int> level(n, -1);
    std::vector<ThreadStats> thread_stats(thread_count);
    std::vector<std::mutex> vertex_mutex(n);

    if (source >= n) {
        return {level, {}, {}};
    }

    std::vector<VertexId> frontier {source};
    level[source] = 0;
    std::size_t current_level = 0;
    const auto global_start = std::chrono::steady_clock::now();

    while (!frontier.empty()) {
        const auto ranges = partition_range(frontier.size(), thread_count);
        std::vector<std::vector<VertexId>> local_next(thread_count);
        std::vector<std::thread> workers;
        workers.reserve(thread_count);

        for (std::size_t worker = 0; worker < thread_count; ++worker) {
            const auto [begin, end] = ranges[worker];
            workers.emplace_back([&, worker, begin, end] {
                for (std::size_t index = begin; index < end; ++index) {
                    const VertexId vertex = frontier[index];
                    for (const auto neighbor : graph.adjacency[vertex]) {
                        const auto compute_start = std::chrono::steady_clock::now();
                        thread_stats[worker].compute_seconds += seconds_since(compute_start);

                        const auto sync_start = std::chrono::steady_clock::now();
                        {
                            std::lock_guard<std::mutex> guard(vertex_mutex[neighbor]);
                            if (level[neighbor] == -1) {
                                level[neighbor] = static_cast<int>(current_level + 1);
                                local_next[worker].push_back(neighbor);
                            }
                        }
                        thread_stats[worker].atomic_seconds += seconds_since(sync_start);
                        thread_stats[worker].traversed_edges += 1;
                    }
                }
            });
        }

        for (auto& worker : workers) {
            worker.join();
        }

        frontier.clear();
        for (const auto& chunk : local_next) {
            frontier.insert(frontier.end(), chunk.begin(), chunk.end());
        }
        ++current_level;
    }

    const double elapsed = seconds_since(global_start);
    const double atomic_seconds = std::accumulate(thread_stats.begin(), thread_stats.end(), 0.0,
                                                  [](double acc, const ThreadStats& stats) {
                                                      return acc + stats.atomic_seconds;
                                                  });
    const double compute_seconds = std::accumulate(thread_stats.begin(), thread_stats.end(), 0.0,
                                                   [](double acc, const ThreadStats& stats) {
                                                       return acc + stats.compute_seconds;
                                                   });
    const std::size_t traversed_edges = std::accumulate(thread_stats.begin(), thread_stats.end(), std::size_t {0},
                                                        [](std::size_t acc, const ThreadStats& stats) {
                                                            return acc + stats.traversed_edges;
                                                        });

    return {level, {}, finalize_metrics(elapsed, traversed_edges, atomic_seconds, compute_seconds, thread_count)};
}

BenchmarkReport benchmark_pagerank(const CSRGraph& csr_graph,
                                   const AdjacencyGraph& adjacency_graph,
                                   std::size_t iterations,
                                   std::size_t thread_count,
                                   double damping,
                                   std::size_t trials) {
    trials = std::max<std::size_t>(1, trials);
    std::vector<MetricSnapshot> baseline_samples;
    std::vector<MetricSnapshot> optimized_samples;
    baseline_samples.reserve(trials);
    optimized_samples.reserve(trials);

    for (std::size_t trial = 0; trial < trials; ++trial) {
        baseline_samples.push_back(run_baseline_pagerank(adjacency_graph, iterations, damping).metrics);
        optimized_samples.push_back(run_pagerank(csr_graph, iterations, thread_count, damping).metrics);
    }

    auto baseline_entry = summarize_entry("Adjacency Serial Baseline", baseline_samples);
    auto optimized_entry = summarize_entry("CSR Lock-Free Parallel", optimized_samples);
    const double baseline_elapsed = std::max(baseline_entry.metrics.elapsed_seconds, 1e-12);
    const double optimized_elapsed = std::max(optimized_entry.metrics.elapsed_seconds, 1e-12);
    baseline_entry.speedup_vs_baseline = 1.0;
    optimized_entry.speedup_vs_baseline = baseline_elapsed / optimized_elapsed;

    return {"pagerank", {baseline_entry, optimized_entry}};
}

BenchmarkReport benchmark_bfs(const CSRGraph& csr_graph,
                              const AdjacencyGraph& adjacency_graph,
                              VertexId source,
                              std::size_t thread_count,
                              std::size_t trials) {
    trials = std::max<std::size_t>(1, trials);
    std::vector<MetricSnapshot> baseline_samples;
    std::vector<MetricSnapshot> optimized_samples;
    baseline_samples.reserve(trials);
    optimized_samples.reserve(trials);

    for (std::size_t trial = 0; trial < trials; ++trial) {
        baseline_samples.push_back(run_mutex_bfs(adjacency_graph, source, thread_count).metrics);
        optimized_samples.push_back(run_parallel_bfs(csr_graph, source, thread_count, false).metrics);
    }

    auto baseline_entry = summarize_entry("Adjacency + Mutex Baseline", baseline_samples);
    auto optimized_entry = summarize_entry("CSR + Atomics", optimized_samples);
    const double baseline_elapsed = std::max(baseline_entry.metrics.elapsed_seconds, 1e-12);
    const double optimized_elapsed = std::max(optimized_entry.metrics.elapsed_seconds, 1e-12);
    baseline_entry.speedup_vs_baseline = 1.0;
    optimized_entry.speedup_vs_baseline = baseline_elapsed / optimized_elapsed;

    return {"bfs", {baseline_entry, optimized_entry}};
}

}  // namespace amb
