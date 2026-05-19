import { readdir, readFile, writeFile } from "node:fs/promises";
import path from "node:path";

const resultDir = process.argv[2] || process.env.RESULT_DIR;

if (!resultDir) {
  console.error("usage: node scripts/benchmark_report.mjs <benchmark-result-dir>");
  process.exit(2);
}

function numberValue(value) {
  if (value === undefined || value === null || value === "") return null;
  const parsed = Number.parseFloat(String(value).replace(/,/g, ""));
  return Number.isFinite(parsed) ? parsed : null;
}

function findLine(text, pattern) {
  return text.split(/\r?\n/).find((line) => pattern.test(line)) || "";
}

function parseWrk(name, text) {
  if (!text.includes("Running") || !text.includes("Requests/sec:")) return null;
  const running = findLine(text, /^Running /);
  const latency = findLine(text, /^\s+Latency\s+/);
  const requests = text.match(/Requests\/sec:\s+([\d.]+)/);
  const transfer = text.match(/Transfer\/sec:\s+([^\n]+)/);
  const total = text.match(/(\d+)\s+requests in\s+([^,]+),\s+([^\n]+)\s+read/);

  return {
    kind: "wrk",
    name,
    target: running.replace(/^Running\s+\S+\s+test\s+@\s+/, "").trim() || null,
    requestsPerSecond: numberValue(requests?.[1]),
    averageLatency: latency.trim().split(/\s+/)[1] || null,
    latencyStdev: latency.trim().split(/\s+/)[2] || null,
    maxLatency: latency.trim().split(/\s+/)[3] || null,
    transferPerSecond: transfer?.[1]?.trim() || null,
    totalRequests: numberValue(total?.[1]),
    totalDuration: total?.[2]?.trim() || null,
    totalRead: total?.[3]?.trim() || null
  };
}

function parseHey(name, text) {
  if (!text.includes("Summary:") || !text.includes("Requests/sec:")) return null;
  const requests = text.match(/Requests\/sec:\s+([\d.]+)/);
  const average = text.match(/Average:\s+([\d.]+)\s+secs/);
  const fastest = text.match(/Fastest:\s+([\d.]+)\s+secs/);
  const slowest = text.match(/Slowest:\s+([\d.]+)\s+secs/);
  const p95 = text.match(/95%\s+in\s+([\d.]+)\s+secs/);
  const p99 = text.match(/99%\s+in\s+([\d.]+)\s+secs/);
  const statuses = {};
  for (const match of text.matchAll(/\[(\d+)\]\s+(\d+)\s+responses/g)) {
    statuses[match[1]] = Number.parseInt(match[2], 10);
  }

  return {
    kind: "hey",
    name,
    requestsPerSecond: numberValue(requests?.[1]),
    averageLatencySeconds: numberValue(average?.[1]),
    fastestSeconds: numberValue(fastest?.[1]),
    slowestSeconds: numberValue(slowest?.[1]),
    p95Seconds: numberValue(p95?.[1]),
    p99Seconds: numberValue(p99?.[1]),
    statuses
  };
}

function parseWs(name, text) {
  if (!text.includes('"clients"') || !text.includes('"welcomed"')) return null;
  const start = text.indexOf("{");
  const end = text.lastIndexOf("}");
  if (start < 0 || end <= start) return null;
  try {
    const summary = JSON.parse(text.slice(start, end + 1));
    return {
      kind: "websocket",
      name,
      ...summary,
      ok: text.includes("load test ok")
    };
  } catch {
    return null;
  }
}

function parseLog(name, text) {
  return parseWs(name, text) || parseWrk(name, text) || parseHey(name, text) || {
    kind: "unknown",
    name
  };
}

function fmt(value, suffix = "") {
  if (value === null || value === undefined || value === "") return "-";
  if (typeof value === "number") {
    return `${new Intl.NumberFormat("en-US", { maximumFractionDigits: 2 }).format(value)}${suffix}`;
  }
  return `${value}${suffix}`;
}

function markdownTable(headers, rows) {
  if (!rows.length) return "";
  return [
    `| ${headers.join(" | ")} |`,
    `| ${headers.map(() => "---").join(" | ")} |`,
    ...rows.map((row) => `| ${row.join(" | ")} |`)
  ].join("\n");
}

function bestBy(items, key) {
  return items
    .filter((item) => typeof item[key] === "number")
    .sort((a, b) => b[key] - a[key])[0] || null;
}

function buildMarkdown(report) {
  const httpRows = report.results
    .filter((item) => item.kind === "wrk" || item.kind === "hey")
    .map((item) => [
      item.name,
      item.kind,
      fmt(item.requestsPerSecond),
      item.kind === "wrk" ? fmt(item.averageLatency) : fmt(item.averageLatencySeconds, "s"),
      item.kind === "wrk" ? fmt(item.maxLatency) : fmt(item.p95Seconds, "s"),
      item.kind === "hey" ? Object.entries(item.statuses || {}).map(([code, count]) => `${code}:${count}`).join(", ") || "-" : fmt(item.transferPerSecond)
    ]);

  const wsRows = report.results
    .filter((item) => item.kind === "websocket")
    .map((item) => [
      item.name,
      `${fmt(item.welcomed)}/${fmt(item.clients)}`,
      fmt(item.rooms),
      fmt(item.snapshots),
      fmt(item.latencyMs?.p95, "ms"),
      fmt(item.serverErrors),
      fmt(item.expectedDefensiveErrors),
      fmt(item.unexpectedServerErrors),
      item.ok ? "yes" : "no"
    ]);

  const bestHttp = bestBy(report.results.filter((item) => item.kind === "wrk" || item.kind === "hey"), "requestsPerSecond");
  const largestWs = report.results
    .filter((item) => item.kind === "websocket" && typeof item.clients === "number")
    .sort((a, b) => b.clients - a.clients)[0] || null;

  const lines = [
    "# VixArena Benchmark Report",
    "",
    `Generated at: ${report.generatedAt}`,
    `Result directory: \`${report.resultDir}\``,
    "",
    "## Highlights",
    "",
    `- Best HTTP throughput: ${bestHttp ? `${fmt(bestHttp.requestsPerSecond)} RPS (${bestHttp.name})` : "-"}`,
    `- Largest WebSocket run: ${largestWs ? `${fmt(largestWs.welcomed)}/${fmt(largestWs.clients)} welcomed across ${fmt(largestWs.rooms)} room(s), p95 ${fmt(largestWs.latencyMs?.p95, "ms")}` : "-"}`,
    "",
    "## HTTP",
    "",
    markdownTable(["Log", "Tool", "RPS", "Avg latency", "P95/max latency", "Status/transfer"], httpRows) || "_No HTTP benchmark logs found._",
    "",
    "## WebSocket",
    "",
    markdownTable(["Log", "Welcomed", "Rooms", "Snapshots", "p95", "Server errors", "Expected defensive", "Unexpected", "OK"], wsRows) || "_No WebSocket benchmark logs found._",
    "",
    "## Notes",
    "",
    "- `expectedDefensiveErrors` are known rate-limit or gameplay cooldown responses under aggressive load.",
    "- `unexpectedServerErrors`, client/protocol errors, missing welcomes, or missing snapshots should be treated as failures.",
    "- Direct-origin results are more useful for backend capacity than Cloudflare-edge results."
  ];

  return `${lines.join("\n")}\n`;
}

const entries = await readdir(resultDir, { withFileTypes: true });
const logs = entries
  .filter((entry) => entry.isFile() && entry.name.endsWith(".log"))
  .map((entry) => entry.name)
  .sort();

const results = [];
for (const log of logs) {
  const text = await readFile(path.join(resultDir, log), "utf8");
  results.push(parseLog(log.replace(/\.log$/, ""), text));
}

const report = {
  generatedAt: new Date().toISOString(),
  resultDir,
  results
};

await writeFile(path.join(resultDir, "report.json"), `${JSON.stringify(report, null, 2)}\n`);
await writeFile(path.join(resultDir, "report.md"), buildMarkdown(report));
console.log(`benchmark report written: ${path.join(resultDir, "report.md")}`);
