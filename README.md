# AMB: High-Performance Concurrent Graph Processing Engine

AMB is a from-scratch systems project that demonstrates a cache-friendly, CSR-backed graph execution engine in C++20. It focuses on the core ideas in your prompt:

- contiguous CSR storage for graph topology
- lock-free style vertex/property updates with atomics
- parallel PageRank and BFS execution across a worker partition
- throughput-oriented metrics such as TEPS, synchronization cost, estimated LLC pressure, and memory bandwidth
- explicit baseline comparisons so the project shows measured improvement
- a local browser demo that visualizes BFS activity over time

## Project Layout

- `include/graph.hpp`: shared graph structures, metrics, and algorithm entry points
- `src/graph.cpp`: CSR loader, atomic double helper, JSON emitters
- `src/algorithms.cpp`: parallel PageRank and BFS implementations, plus baseline comparisons
- `src/main.cpp`: CLI runner
- `tools/server.py`: local demo server for the browser UI
- `tools/generate_powerlaw_graph.py`: deterministic benchmark-graph generator
- `web/`: browser visualization and controls
- `data/sample_graph.edgelist`: small skewed sample graph

## Build

```bash
make
```

## Run Native Algorithms

PageRank:

```bash
./build/amb_graph --graph data/sample_graph.edgelist --algorithm pagerank --iterations 20 --threads 4
```

BFS with trace output for the UI:

```bash
./build/amb_graph --graph data/sample_graph.edgelist --algorithm bfs --source 0 --threads 4 --emit-trace web/trace.json --undirected
```

Benchmark comparison:

```bash
python3 tools/generate_powerlaw_graph.py --output data/powerlaw_graph.edgelist --vertices 4096 --fanout 12
./build/amb_graph --graph data/powerlaw_graph.edgelist --algorithm bfs --source 0 --threads 4 --trials 5 --benchmark --emit-benchmark web/benchmark.json --undirected
```

For more meaningful comparisons, run the benchmark mode on the larger `data/powerlaw_graph.edgelist` graph instead of the tiny visualization graph.

## Demo UI

Build the engine first, then start the local server:

```bash
make
python3 tools/server.py
```

Open `http://127.0.0.1:8000` and run the BFS demo. The UI invokes the native engine, reads back `web/trace.json`, and animates the active frontier.

Important UI behavior:

- `Parallel BFS` animates the graph traversal on the canvas
- `PageRank` does not animate the graph yet
- `PageRank` still updates the log, metrics, and benchmark comparison areas

## Hosting

This project is not a static site. The browser UI depends on a Python web server, and that server runs the compiled C++ engine for `/api/run`. That means you should host it as a web service, not on GitHub Pages.

The repository is now deployment-ready for Docker-based hosting:

```bash
docker build -t amb-demo .
docker run -p 8000:8000 amb-demo
```

Then open `http://127.0.0.1:8000`.

The server now reads:

- `HOST` for bind address
- `PORT` for the web-service port

So it can run on typical cloud platforms that inject a port automatically.

## Notes on Metrics

This repository reports:

- `TEPS`: traversed edges per second during the algorithm run
- `atomic_seconds`: sampled wall time spent in atomic update sections
- `compute_seconds`: sampled wall time spent outside atomic sections
- `synchronization_ratio`: `atomic_seconds / (atomic_seconds + compute_seconds)`
- `estimated_llc_miss_rate`: a derived estimate based on contention and thread count for demo purposes
- `estimated_bandwidth_gbps`: approximate bandwidth based on bytes touched per traversed edge

This repository also compares:

- `Adjacency Serial Baseline` vs `CSR Lock-Free Parallel` for PageRank
- `Adjacency + Mutex Baseline` vs `CSR + Atomics` for BFS

The speedup value tells you how much faster the optimized engine is than the baseline on the same workload.

To reduce noise, benchmark mode now runs repeated trials and reports average TEPS and average elapsed time, along with min/max elapsed time and per-trial samples.

For a research-grade benchmark, the next step would be to integrate hardware counter sampling via platform tools such as `perf`, VTune, or Instruments and feed those counters back into the UI.
