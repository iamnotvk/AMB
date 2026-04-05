#include "graph.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <ios>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace amb {

namespace {

double now_seconds() {
    using clock = std::chrono::steady_clock;
    const auto now = clock::now().time_since_epoch();
    return std::chrono::duration<double>(now).count();
}

std::uint64_t double_to_bits(double value) {
    return std::bit_cast<std::uint64_t>(value);
}

double bits_to_double(std::uint64_t value) {
    return std::bit_cast<double>(value);
}

}  // namespace

std::size_t CSRGraph::degree(VertexId vertex) const {
    return static_cast<std::size_t>(offsets[vertex + 1] - offsets[vertex]);
}

std::pair<const VertexId*, const VertexId*> CSRGraph::neighbors(VertexId vertex) const {
    const auto begin = static_cast<std::size_t>(offsets[vertex]);
    const auto end = static_cast<std::size_t>(offsets[vertex + 1]);
    return {edges.data() + begin, edges.data() + end};
}

CSRGraph CSRGraph::from_edge_list_file(const std::string& path, bool make_undirected) {
    const double start = now_seconds();
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Failed to open graph file: " + path);
    }

    std::vector<std::pair<VertexId, VertexId>> edge_list;
    edge_list.reserve(1024);

    std::string line;
    VertexId max_vertex = 0;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream row(line);
        VertexId src = 0;
        VertexId dst = 0;
        if (!(row >> src >> dst)) {
            continue;
        }
        edge_list.emplace_back(src, dst);
        max_vertex = std::max(max_vertex, std::max(src, dst));
        if (make_undirected && src != dst) {
            edge_list.emplace_back(dst, src);
        }
    }

    CSRGraph graph;
    graph.vertex_count = edge_list.empty() ? 0 : static_cast<std::size_t>(max_vertex) + 1;
    graph.edge_count = edge_list.size();
    graph.offsets.assign(graph.vertex_count + 1, 0);
    graph.out_degree.assign(graph.vertex_count, 0);

    for (const auto& [src, _] : edge_list) {
        graph.offsets[src + 1] += 1;
        graph.out_degree[src] += 1;
    }

    for (std::size_t i = 1; i < graph.offsets.size(); ++i) {
        graph.offsets[i] += graph.offsets[i - 1];
    }

    graph.edges.assign(graph.edge_count, 0);
    auto cursor = graph.offsets;
    for (const auto& [src, dst] : edge_list) {
        graph.edges[cursor[src]++] = dst;
    }

    const double elapsed = now_seconds() - start;
    std::cout << std::fixed << std::setprecision(6)
              << "Loaded CSR graph in " << elapsed << "s with "
              << graph.vertex_count << " vertices and "
              << graph.edge_count << " edges\n";

    return graph;
}

AdjacencyGraph AdjacencyGraph::from_csr(const CSRGraph& graph) {
    AdjacencyGraph adjacency_graph;
    adjacency_graph.adjacency.resize(graph.vertex_count);
    for (VertexId vertex = 0; vertex < graph.vertex_count; ++vertex) {
        const auto [begin, end] = graph.neighbors(vertex);
        adjacency_graph.adjacency[vertex].assign(begin, end);
    }
    return adjacency_graph;
}

std::size_t AdjacencyGraph::vertex_count() const {
    return adjacency.size();
}

std::size_t AdjacencyGraph::edge_count() const {
    std::size_t total = 0;
    for (const auto& neighbors : adjacency) {
        total += neighbors.size();
    }
    return total;
}

AtomicDoubleArray::AtomicDoubleArray(std::size_t size) : storage(size) {
    reset(0.0);
}

void AtomicDoubleArray::store(std::size_t index, double value, std::memory_order order) {
    storage[index].store(double_to_bits(value), order);
}

double AtomicDoubleArray::load(std::size_t index, std::memory_order order) const {
    return bits_to_double(storage[index].load(order));
}

double AtomicDoubleArray::fetch_add(std::size_t index, double delta, std::memory_order order) {
    auto& cell = storage[index];
    std::uint64_t observed = cell.load(order);
    while (true) {
        const double current = bits_to_double(observed);
        const double desired_value = current + delta;
        const std::uint64_t desired = double_to_bits(desired_value);
        if (cell.compare_exchange_weak(observed, desired, order, std::memory_order_relaxed)) {
            return current;
        }
    }
}

void AtomicDoubleArray::reset(double value, std::memory_order order) {
    const auto bits = double_to_bits(value);
    for (auto& cell : storage) {
        cell.store(bits, order);
    }
}

std::string build_metrics_json(const MetricSnapshot& metrics) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(6);
    out << "{";
    out << "\"elapsed_seconds\":" << metrics.elapsed_seconds << ",";
    out << "\"teps\":" << metrics.teps << ",";
    out << "\"atomic_seconds\":" << metrics.atomic_seconds << ",";
    out << "\"compute_seconds\":" << metrics.compute_seconds << ",";
    out << "\"synchronization_ratio\":" << metrics.synchronization_ratio << ",";
    out << "\"estimated_llc_miss_rate\":" << metrics.estimated_llc_miss_rate << ",";
    out << "\"estimated_bandwidth_gbps\":" << metrics.estimated_bandwidth_gbps;
    out << "}";
    return out.str();
}

std::string build_benchmark_json(const BenchmarkReport& report) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(6);
    out << "{";
    out << "\"algorithm\":\"" << report.algorithm << "\",";
    out << "\"entries\":[";
    for (std::size_t i = 0; i < report.entries.size(); ++i) {
        const auto& entry = report.entries[i];
        out << "{";
        out << "\"label\":\"" << entry.label << "\",";
        out << "\"metrics\":" << build_metrics_json(entry.metrics) << ",";
        out << "\"speedup_vs_baseline\":" << entry.speedup_vs_baseline << ",";
        out << "\"trial_count\":" << entry.trial_count << ",";
        out << "\"min_elapsed_seconds\":" << entry.min_elapsed_seconds << ",";
        out << "\"max_elapsed_seconds\":" << entry.max_elapsed_seconds << ",";
        out << "\"elapsed_samples\":[";
        for (std::size_t j = 0; j < entry.elapsed_samples.size(); ++j) {
            out << entry.elapsed_samples[j];
            if (j + 1 != entry.elapsed_samples.size()) {
                out << ",";
            }
        }
        out << "],";
        out << "\"teps_samples\":[";
        for (std::size_t j = 0; j < entry.teps_samples.size(); ++j) {
            out << entry.teps_samples[j];
            if (j + 1 != entry.teps_samples.size()) {
                out << ",";
            }
        }
        out << "]";
        out << "}";
        if (i + 1 != report.entries.size()) {
            out << ",";
        }
    }
    out << "]";
    out << "}";
    return out.str();
}

std::string build_trace_json(const CSRGraph& graph, const BFSResult& bfs) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"vertexCount\": " << graph.vertex_count << ",\n";
    out << "  \"edgeCount\": " << graph.edge_count << ",\n";
    out << "  \"metrics\": " << build_metrics_json(bfs.metrics) << ",\n";
    out << "  \"nodes\": [\n";
    for (std::size_t vertex = 0; vertex < graph.vertex_count; ++vertex) {
        out << "    {\"id\": " << vertex << ", \"level\": " << bfs.level[vertex] << "}";
        out << (vertex + 1 == graph.vertex_count ? "\n" : ",\n");
    }
    out << "  ],\n";
    out << "  \"edges\": [\n";
    std::size_t edge_index = 0;
    for (VertexId src = 0; src < graph.vertex_count; ++src) {
        const auto [begin, end] = graph.neighbors(src);
        for (auto it = begin; it != end; ++it) {
            out << "    {\"source\": " << src << ", \"target\": " << *it << "}";
            ++edge_index;
            out << (edge_index == graph.edge_count ? "\n" : ",\n");
        }
    }
    out << "  ],\n";
    out << "  \"trace\": [\n";
    for (std::size_t i = 0; i < bfs.trace.size(); ++i) {
        const auto& event = bfs.trace[i];
        out << "    {\"level\": " << event.level << ", \"vertex\": " << event.vertex << "}";
        out << (i + 1 == bfs.trace.size() ? "\n" : ",\n");
    }
    out << "  ]\n";
    out << "}\n";
    return out.str();
}

}  // namespace amb
