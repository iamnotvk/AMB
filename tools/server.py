#!/usr/bin/env python3

import json
import os
import pathlib
import subprocess
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse


ROOT = pathlib.Path(__file__).resolve().parents[1]
WEB_ROOT = ROOT / "web"
ENGINE = ROOT / "build" / "amb_graph"
DEFAULT_GRAPH = ROOT / "data" / "sample_graph.edgelist"
BENCHMARK_PATH = WEB_ROOT / "benchmark.json"
BENCHMARK_GRAPH = ROOT / "data" / "powerlaw_graph.edgelist"


def ensure_benchmark_graph():
    if BENCHMARK_GRAPH.exists():
        return
    subprocess.run(
        [
            "python3",
            str(ROOT / "tools" / "generate_powerlaw_graph.py"),
            "--output",
            str(BENCHMARK_GRAPH),
            "--vertices",
            "4096",
            "--fanout",
            "12",
        ],
        cwd=ROOT,
        check=True,
    )


class DemoHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(WEB_ROOT), **kwargs)

    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path == "/api/run":
            self.handle_run(parsed)
            return
        if parsed.path == "/api/trace":
            self.handle_trace()
            return
        if parsed.path == "/api/benchmark":
            self.handle_benchmark()
            return
        super().do_GET()

    def handle_run(self, parsed):
        params = parse_qs(parsed.query)
        algorithm = params.get("algorithm", ["bfs"])[0]
        threads = params.get("threads", ["4"])[0]
        source = params.get("source", ["0"])[0]
        trials = params.get("trials", ["5"])[0]
        ensure_benchmark_graph()

        trace_path = WEB_ROOT / "trace.json"
        command = [
            str(ENGINE),
            "--graph",
            str(DEFAULT_GRAPH),
            "--algorithm",
            algorithm,
            "--threads",
            threads,
        ]
        if algorithm == "bfs":
            command += ["--source", source, "--emit-trace", str(trace_path), "--undirected"]
        else:
            command += ["--iterations", "20"]

        completed = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, check=False)
        benchmark_command = [
            str(ENGINE),
            "--graph",
            str(BENCHMARK_GRAPH),
            "--algorithm",
            algorithm,
            "--threads",
            threads,
            "--trials",
            trials,
            "--benchmark",
            "--emit-benchmark",
            str(BENCHMARK_PATH),
        ]
        if algorithm == "bfs":
            benchmark_command += ["--source", "0", "--undirected"]
        else:
            benchmark_command += ["--iterations", "20"]
        benchmark_completed = subprocess.run(benchmark_command, cwd=ROOT, capture_output=True, text=True, check=False)
        payload = {
            "ok": completed.returncode == 0 and benchmark_completed.returncode == 0,
            "stdout": completed.stdout,
            "stderr": "\n".join(part for part in [completed.stderr, benchmark_completed.stderr] if part),
            "traceReady": trace_path.exists(),
            "benchmarkReady": BENCHMARK_PATH.exists(),
        }
        self.send_json(payload, 200 if payload["ok"] else 500)

    def handle_trace(self):
        trace_path = WEB_ROOT / "trace.json"
        if not trace_path.exists():
            self.send_json({"ok": False, "error": "trace.json not found"}, 404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(trace_path.read_bytes())

    def handle_benchmark(self):
        if not BENCHMARK_PATH.exists():
            self.send_json({"ok": False, "error": "benchmark.json not found"}, 404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(BENCHMARK_PATH.read_bytes())

    def send_json(self, payload, status):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    host = os.environ.get("HOST", "0.0.0.0")
    port = int(os.environ.get("PORT", "8000"))
    server = ThreadingHTTPServer((host, port), DemoHandler)
    print(f"AMB demo server listening on http://{host}:{port}")
    server.serve_forever()


if __name__ == "__main__":
    main()
