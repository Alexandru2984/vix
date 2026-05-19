import { readFile, writeFile } from "node:fs/promises";
import path from "node:path";

const [baseDir, candidateDir] = process.argv.slice(2);

if (!baseDir || !candidateDir) {
  console.error("usage: node scripts/benchmark_compare.mjs <baseline-result-dir> <candidate-result-dir>");
  process.exit(2);
}

function fmt(value, suffix = "") {
  if (value === null || value === undefined || Number.isNaN(value)) return "-";
  if (typeof value === "number") {
    return `${new Intl.NumberFormat("en-US", { maximumFractionDigits: 2 }).format(value)}${suffix}`;
  }
  return `${value}${suffix}`;
}

function pct(from, to) {
  if (typeof from !== "number" || typeof to !== "number" || from === 0) return null;
  return ((to - from) / from) * 100;
}

function trend(delta, higherIsBetter = true) {
  if (delta === null || Math.abs(delta) < 0.01) return "flat";
  const better = higherIsBetter ? delta > 0 : delta < 0;
  return better ? "better" : "worse";
}

function markdownTable(headers, rows) {
  if (!rows.length) return "";
  return [
    `| ${headers.join(" | ")} |`,
    `| ${headers.map(() => "---").join(" | ")} |`,
    ...rows.map((row) => `| ${row.join(" | ")} |`)
  ].join("\n");
}

async function readReport(dir) {
  const file = path.join(dir, "report.json");
  const report = JSON.parse(await readFile(file, "utf8"));
  return { ...report, dir };
}

function httpResults(report) {
  return (report.results || []).filter((item) => item.kind === "wrk" || item.kind === "hey");
}

function wsResults(report) {
  return (report.results || []).filter((item) => item.kind === "websocket");
}

function bestHttp(report) {
  return httpResults(report)
    .filter((item) => typeof item.requestsPerSecond === "number")
    .sort((a, b) => b.requestsPerSecond - a.requestsPerSecond)[0] || null;
}

function largestWs(report) {
  return wsResults(report)
    .filter((item) => typeof item.clients === "number")
    .sort((a, b) => b.clients - a.clients || (b.welcomed || 0) - (a.welcomed || 0))[0] || null;
}

function byName(items) {
  return new Map(items.map((item) => [item.name, item]));
}

const baseline = await readReport(baseDir);
const candidate = await readReport(candidateDir);

const baseBestHttp = bestHttp(baseline);
const candidateBestHttp = bestHttp(candidate);
const baseLargestWs = largestWs(baseline);
const candidateLargestWs = largestWs(candidate);

const httpRows = [];
const baseHttpByName = byName(httpResults(baseline));
const candidateHttpByName = byName(httpResults(candidate));
for (const name of [...new Set([...baseHttpByName.keys(), ...candidateHttpByName.keys()])].sort()) {
  const base = baseHttpByName.get(name);
  const current = candidateHttpByName.get(name);
  const delta = pct(base?.requestsPerSecond, current?.requestsPerSecond);
  httpRows.push([
    name,
    fmt(base?.requestsPerSecond),
    fmt(current?.requestsPerSecond),
    fmt(delta, "%"),
    trend(delta, true)
  ]);
}

const wsRows = [];
const baseWsByName = byName(wsResults(baseline));
const candidateWsByName = byName(wsResults(candidate));
for (const name of [...new Set([...baseWsByName.keys(), ...candidateWsByName.keys()])].sort()) {
  const base = baseWsByName.get(name);
  const current = candidateWsByName.get(name);
  const p95Delta = pct(base?.latencyMs?.p95, current?.latencyMs?.p95);
  wsRows.push([
    name,
    `${fmt(base?.welcomed)}/${fmt(base?.clients)}`,
    `${fmt(current?.welcomed)}/${fmt(current?.clients)}`,
    fmt(base?.latencyMs?.p95, "ms"),
    fmt(current?.latencyMs?.p95, "ms"),
    fmt(p95Delta, "%"),
    fmt(base?.unexpectedServerErrors),
    fmt(current?.unexpectedServerErrors),
    trend(p95Delta, false)
  ]);
}

const bestHttpDelta = pct(baseBestHttp?.requestsPerSecond, candidateBestHttp?.requestsPerSecond);
const largestWsP95Delta = pct(baseLargestWs?.latencyMs?.p95, candidateLargestWs?.latencyMs?.p95);

const comparison = {
  generatedAt: new Date().toISOString(),
  baseline: {
    dir: baseline.dir,
    generatedAt: baseline.generatedAt,
    bestHttp: baseBestHttp,
    largestWebSocket: baseLargestWs
  },
  candidate: {
    dir: candidate.dir,
    generatedAt: candidate.generatedAt,
    bestHttp: candidateBestHttp,
    largestWebSocket: candidateLargestWs
  },
  deltas: {
    bestHttpRequestsPerSecondPercent: bestHttpDelta,
    largestWebSocketP95LatencyPercent: largestWsP95Delta
  }
};

const lines = [
  "# VixArena Benchmark Comparison",
  "",
  `Generated at: ${comparison.generatedAt}`,
  `Baseline: \`${baseline.dir}\``,
  `Candidate: \`${candidate.dir}\``,
  "",
  "## Highlights",
  "",
  `- Best HTTP RPS: ${fmt(baseBestHttp?.requestsPerSecond)} -> ${fmt(candidateBestHttp?.requestsPerSecond)} (${fmt(bestHttpDelta, "%")}, ${trend(bestHttpDelta, true)})`,
  `- Largest WS run: ${fmt(baseLargestWs?.welcomed)}/${fmt(baseLargestWs?.clients)} -> ${fmt(candidateLargestWs?.welcomed)}/${fmt(candidateLargestWs?.clients)}`,
  `- Largest WS p95 latency: ${fmt(baseLargestWs?.latencyMs?.p95, "ms")} -> ${fmt(candidateLargestWs?.latencyMs?.p95, "ms")} (${fmt(largestWsP95Delta, "%")}, ${trend(largestWsP95Delta, false)})`,
  "",
  "## Matching HTTP Tests",
  "",
  markdownTable(["Log", "Baseline RPS", "Candidate RPS", "Delta", "Trend"], httpRows) || "_No HTTP results to compare._",
  "",
  "## Matching WebSocket Tests",
  "",
  markdownTable(["Log", "Baseline welcomed", "Candidate welcomed", "Baseline p95", "Candidate p95", "p95 delta", "Baseline unexpected", "Candidate unexpected", "Trend"], wsRows) || "_No WebSocket results to compare._",
  "",
  "## Notes",
  "",
  "- Positive HTTP RPS delta is better.",
  "- Negative WebSocket p95 latency delta is better.",
  "- Compare tests with the same duration, origin path, benchmark profile, and client VPS whenever possible."
];

const outDir = candidate.dir;
await writeFile(path.join(outDir, "comparison.json"), `${JSON.stringify(comparison, null, 2)}\n`);
await writeFile(path.join(outDir, "comparison.md"), `${lines.join("\n")}\n`);
console.log(`benchmark comparison written: ${path.join(outDir, "comparison.md")}`);
