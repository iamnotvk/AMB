const runButton = document.getElementById("run-button");
const threadSlider = document.getElementById("threads");
const threadLabel = document.getElementById("thread-label");
const sourceInput = document.getElementById("source");
const algorithmSelect = document.getElementById("algorithm");
const logPanel = document.getElementById("log-panel");
const canvas = document.getElementById("graph-canvas");
const visualTitle = document.getElementById("visual-title");
const visualSubtitle = document.getElementById("visual-subtitle");
const visualOverlay = document.getElementById("visual-overlay");
const benchmarkTable = document.getElementById("benchmark-table");
const improvementSummary = document.getElementById("improvement-summary");
const benchmarkMeta = document.getElementById("benchmark-meta");
const ctx = canvas.getContext("2d");

threadSlider.addEventListener("input", () => {
  threadLabel.textContent = threadSlider.value;
});

algorithmSelect.addEventListener("change", () => {
  setVisualMode(algorithmSelect.value);
});

const metricsEls = {
  teps: document.getElementById("metric-teps"),
  elapsed: document.getElementById("metric-elapsed"),
  sync: document.getElementById("metric-sync"),
  llc: document.getElementById("metric-llc"),
  bandwidth: document.getElementById("metric-bandwidth"),
};

let layout = null;

function polarLayout(count, width, height) {
  const radius = Math.min(width, height) * 0.36;
  const cx = width / 2;
  const cy = height / 2;
  return Array.from({ length: count }, (_, i) => {
    const angle = (Math.PI * 2 * i) / Math.max(1, count);
    return {
      x: cx + Math.cos(angle) * radius + Math.cos(angle * 3) * 25,
      y: cy + Math.sin(angle) * radius + Math.sin(angle * 2) * 18,
    };
  });
}

function drawGraph(traceData, activeIndex = -1) {
  const { nodes, edges, trace } = traceData;
  if (!layout || layout.length !== nodes.length) {
    layout = polarLayout(nodes.length, canvas.width, canvas.height);
  }

  ctx.clearRect(0, 0, canvas.width, canvas.height);
  ctx.lineWidth = 1.2;
  ctx.strokeStyle = "rgba(149, 194, 255, 0.16)";
  for (const edge of edges) {
    const a = layout[edge.source];
    const b = layout[edge.target];
    ctx.beginPath();
    ctx.moveTo(a.x, a.y);
    ctx.lineTo(b.x, b.y);
    ctx.stroke();
  }

  const activeVertices = new Set(trace.slice(0, activeIndex + 1).map((event) => event.vertex));
  for (const node of nodes) {
    const point = layout[node.id];
    const visited = activeVertices.has(node.id);
    ctx.beginPath();
    ctx.fillStyle = visited ? "#73f0b7" : "rgba(157, 179, 207, 0.55)";
    ctx.shadowColor = visited ? "rgba(115, 240, 183, 0.6)" : "transparent";
    ctx.shadowBlur = visited ? 18 : 0;
    ctx.arc(point.x, point.y, visited ? 9 : 6, 0, Math.PI * 2);
    ctx.fill();
    ctx.shadowBlur = 0;
  }
}

function setVisualMode(algorithm) {
  const isBfs = algorithm === "bfs";
  visualTitle.textContent = isBfs ? "BFS Frontier Playback" : "PageRank Results Overview";
  visualSubtitle.textContent = isBfs
    ? "This view shows which vertices become active at each BFS level."
    : "PageRank currently updates the metrics, engine log, and benchmark cards instead of animating the graph.";
  visualOverlay.hidden = isBfs;
}

function updateMetrics(metrics) {
  metricsEls.teps.textContent = Number(metrics.teps).toFixed(0);
  metricsEls.elapsed.textContent = `${Number(metrics.elapsed_seconds).toFixed(4)} s`;
  metricsEls.sync.textContent = Number(metrics.synchronization_ratio).toFixed(3);
  metricsEls.llc.textContent = `${(Number(metrics.estimated_llc_miss_rate) * 100).toFixed(1)}%`;
  metricsEls.bandwidth.textContent = `${Number(metrics.estimated_bandwidth_gbps).toFixed(3)} GB/s`;
}

function clearMetrics() {
  metricsEls.teps.textContent = "-";
  metricsEls.elapsed.textContent = "-";
  metricsEls.sync.textContent = "-";
  metricsEls.llc.textContent = "-";
  metricsEls.bandwidth.textContent = "-";
}

function renderBenchmark(report) {
  if (!report?.entries?.length) {
    improvementSummary.textContent = "No benchmark results available.";
    benchmarkMeta.textContent = "Benchmarks will report averaged results across repeated trials.";
    benchmarkTable.innerHTML = "";
    return;
  }

  const baseline = report.entries[0];
  const optimized = report.entries[1] ?? baseline;
  benchmarkMeta.textContent = `Averages across ${optimized.trial_count || baseline.trial_count || 1} repeated trials on the benchmark graph.`;
  if (optimized.speedup_vs_baseline >= 1) {
    improvementSummary.textContent = `${optimized.label} is ${Number(optimized.speedup_vs_baseline).toFixed(2)}x faster than ${baseline.label} on this ${report.algorithm.toUpperCase()} run.`;
  } else {
    improvementSummary.textContent = `${optimized.label} is currently slower on this ${report.algorithm.toUpperCase()} run. ${baseline.label} is ${(1 / Math.max(optimized.speedup_vs_baseline, 1e-12)).toFixed(2)}x faster, which means synchronization overhead still dominates here.`;
  }

  benchmarkTable.innerHTML = report.entries
    .map((entry) => {
      const samples = Array.isArray(entry.elapsed_samples) ? entry.elapsed_samples : [];
      const maxSample = Math.max(...samples, 1e-9);
      const bars = samples
        .map((sample) => {
          const height = Math.max(10, (sample / maxSample) * 52);
          return `<span class="trial-bar" style="height:${height}px"></span>`;
        })
        .join("");

      return `
      <div class="benchmark-row">
        <div class="benchmark-cell">
          <strong>${entry.label}</strong>
          <span>${report.algorithm.toUpperCase()} benchmark</span>
          <span class="trial-label">${entry.trial_count} trials, elapsed ${Number(entry.min_elapsed_seconds).toFixed(4)}s to ${Number(entry.max_elapsed_seconds).toFixed(4)}s</span>
        </div>
        <div class="benchmark-cell">
          <span>Avg TEPS: ${Number(entry.metrics.teps).toFixed(0)}</span>
          <span>Avg elapsed: ${Number(entry.metrics.elapsed_seconds).toFixed(4)}s</span>
          <div class="trial-chart">${bars}</div>
        </div>
        <div class="benchmark-cell">
          <span>Speedup: ${Number(entry.speedup_vs_baseline).toFixed(2)}x</span>
          <span>Avg sync: ${Number(entry.metrics.synchronization_ratio).toFixed(3)}</span>
        </div>
      </div>
    `;
    })
    .join("");
}

async function animateTrace(traceData) {
  drawGraph(traceData, -1);
  if (!traceData.trace?.length) {
    return;
  }
  for (let i = 0; i < traceData.trace.length; i += 1) {
    drawGraph(traceData, i);
    await new Promise((resolve) => setTimeout(resolve, 120));
  }
}

async function runDemo() {
  runButton.disabled = true;
  logPanel.textContent = "Running native engine...";
  renderBenchmark(null);

  const algorithm = algorithmSelect.value;
  setVisualMode(algorithm);
  const threads = threadSlider.value;
  const source = sourceInput.value;
  const response = await fetch(`/api/run?algorithm=${algorithm}&threads=${threads}&source=${source}`);
  const payload = await response.json();

  logPanel.textContent = [payload.stdout, payload.stderr].filter(Boolean).join("\n");

  if (!payload.ok) {
    runButton.disabled = false;
    return;
  }

  if (algorithm === "bfs" && payload.traceReady) {
    const traceResponse = await fetch("/api/trace");
    const traceData = await traceResponse.json();
    updateMetrics(traceData.metrics);
    await animateTrace(traceData);
  } else {
    clearMetrics();
  }

  if (payload.benchmarkReady) {
    const benchmarkResponse = await fetch("/api/benchmark");
    const benchmarkData = await benchmarkResponse.json();
    renderBenchmark(benchmarkData);
    if (algorithm !== "bfs" && benchmarkData.entries?.[1]?.metrics) {
      updateMetrics(benchmarkData.entries[1].metrics);
    }
  }

  runButton.disabled = false;
}

runButton.addEventListener("click", () => {
  runDemo().catch((error) => {
    logPanel.textContent = String(error);
    runButton.disabled = false;
  });
});

drawGraph({ nodes: Array.from({ length: 14 }, (_, id) => ({ id })), edges: [], trace: [] });
setVisualMode("bfs");
