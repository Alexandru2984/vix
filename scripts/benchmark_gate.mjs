import { readFile, writeFile } from "node:fs/promises";
import path from "node:path";

const resultDir = process.argv[2] || process.env.RESULT_DIR;

if (!resultDir) {
  console.error("usage: node scripts/benchmark_gate.mjs <benchmark-result-dir>");
  process.exit(2);
}

function envNumber(name, fallback) {
  const raw = process.env[name];
  if (raw === undefined || raw === "") return fallback;
  const parsed = Number.parseFloat(raw);
  return Number.isFinite(parsed) ? parsed : fallback;
}

async function readJsonIfExists(file) {
  try {
    return JSON.parse(await readFile(file, "utf8"));
  } catch {
    return null;
  }
}

function fmt(value, suffix = "") {
  if (value === null || value === undefined || Number.isNaN(value)) return "-";
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

function bestHttp(report) {
  return (report.results || [])
    .filter((item) => (item.kind === "wrk" || item.kind === "hey") && typeof item.requestsPerSecond === "number")
    .sort((a, b) => b.requestsPerSecond - a.requestsPerSecond)[0] || null;
}

function largestWs(report) {
  return (report.results || [])
    .filter((item) => item.kind === "websocket" && typeof item.clients === "number")
    .sort((a, b) => b.clients - a.clients || (b.welcomed || 0) - (a.welcomed || 0))[0] || null;
}

function allWs(report) {
  return (report.results || []).filter((item) => item.kind === "websocket");
}

function passFail(condition) {
  return condition ? "pass" : "fail";
}

const thresholds = {
  minHttpRps: envNumber("BENCH_GATE_MIN_HTTP_RPS", 0),
  minWsClients: envNumber("BENCH_GATE_MIN_WS_CLIENTS", 0),
  maxWsP95Ms: envNumber("BENCH_GATE_MAX_WS_P95_MS", Number.POSITIVE_INFINITY),
  maxUnexpectedServerErrors: envNumber("BENCH_GATE_MAX_UNEXPECTED_SERVER_ERRORS", 0),
  maxProtocolViolationsDelta: envNumber("BENCH_GATE_MAX_PROTOCOL_VIOLATIONS_DELTA", 0),
  maxRejectedConnectionsDelta: envNumber("BENCH_GATE_MAX_REJECTED_CONNECTIONS_DELTA", Number.POSITIVE_INFINITY),
  maxSendFailuresDelta: envNumber("BENCH_GATE_MAX_SEND_FAILURES_DELTA", 0),
  maxOriginCpuPercent: envNumber("BENCH_GATE_ORIGIN_MAX_CPU_PERCENT", Number.POSITIVE_INFINITY),
  maxOriginRssKb: envNumber("BENCH_GATE_ORIGIN_MAX_RSS_KB", Number.POSITIVE_INFINITY),
  maxOriginTickP95Us: envNumber("BENCH_GATE_ORIGIN_MAX_TICK_P95_US", Number.POSITIVE_INFINITY),
  minOriginSamples: envNumber("BENCH_GATE_ORIGIN_MIN_SAMPLES", 0),
  requireOriginMonitor: envNumber("BENCH_GATE_REQUIRE_ORIGIN_MONITOR", 0)
};

const report = JSON.parse(await readFile(path.join(resultDir, "report.json"), "utf8"));
const originReport = await readJsonIfExists(path.join(resultDir, "origin-monitor-report.json"));
const best = bestHttp(report);
const ws = largestWs(report);
const wsResults = allWs(report);
const unexpectedServerErrors = wsResults.reduce((sum, item) => sum + (Number(item.unexpectedServerErrors) || 0), 0);
const protocolViolationsDelta = Number(report.statsDelta?.["websocket.protocolViolations"] || 0);
const rejectedConnectionsDelta = Number(report.statsDelta?.["websocket.rejectedConnections"] || 0);
const sendFailuresDelta = Number(report.statsDelta?.["websocket.sendFailures"] || 0);
const requireOriginMonitor = thresholds.requireOriginMonitor > 0;
const originMonitorPresent = Boolean(originReport);

const checks = [
  {
    name: "best HTTP RPS",
    actual: best?.requestsPerSecond ?? null,
    threshold: `>= ${fmt(thresholds.minHttpRps)}`,
    ok: (best?.requestsPerSecond ?? 0) >= thresholds.minHttpRps
  },
  {
    name: "largest WS clients welcomed",
    actual: ws ? `${fmt(ws.welcomed)}/${fmt(ws.clients)}` : null,
    threshold: `>= ${fmt(thresholds.minWsClients)} clients and welcomed == clients`,
    ok: (ws?.clients ?? 0) >= thresholds.minWsClients && ws?.welcomed === ws?.clients
  },
  {
    name: "largest WS p95 latency",
    actual: ws?.latencyMs?.p95 ?? null,
    threshold: `<= ${fmt(thresholds.maxWsP95Ms, "ms")}`,
    ok: (ws?.latencyMs?.p95 ?? Number.POSITIVE_INFINITY) <= thresholds.maxWsP95Ms
  },
  {
    name: "unexpected server errors",
    actual: unexpectedServerErrors,
    threshold: `<= ${fmt(thresholds.maxUnexpectedServerErrors)}`,
    ok: unexpectedServerErrors <= thresholds.maxUnexpectedServerErrors
  },
  {
    name: "protocol violations delta",
    actual: protocolViolationsDelta,
    threshold: `<= ${fmt(thresholds.maxProtocolViolationsDelta)}`,
    ok: protocolViolationsDelta <= thresholds.maxProtocolViolationsDelta
  },
  {
    name: "rejected connections delta",
    actual: rejectedConnectionsDelta,
    threshold: `<= ${fmt(thresholds.maxRejectedConnectionsDelta)}`,
    ok: rejectedConnectionsDelta <= thresholds.maxRejectedConnectionsDelta
  },
  {
    name: "send failures delta",
    actual: sendFailuresDelta,
    threshold: `<= ${fmt(thresholds.maxSendFailuresDelta)}`,
    ok: sendFailuresDelta <= thresholds.maxSendFailuresDelta
  },
  {
    name: "origin monitor present",
    actual: originMonitorPresent ? "yes" : "no",
    threshold: requireOriginMonitor ? "required" : "optional",
    ok: originMonitorPresent || !requireOriginMonitor
  },
  {
    name: "origin monitor samples",
    actual: originReport?.samples ?? null,
    threshold: `>= ${fmt(thresholds.minOriginSamples)}`,
    ok: !originMonitorPresent ? !requireOriginMonitor : (originReport.samples ?? 0) >= thresholds.minOriginSamples
  },
  {
    name: "origin max CPU",
    actual: originReport?.maxCpuPercent ?? null,
    threshold: `<= ${fmt(thresholds.maxOriginCpuPercent, "%")}`,
    ok: !originMonitorPresent ? !requireOriginMonitor : (originReport.maxCpuPercent ?? Number.POSITIVE_INFINITY) <= thresholds.maxOriginCpuPercent
  },
  {
    name: "origin max RSS",
    actual: originReport?.maxRssKb ?? null,
    threshold: `<= ${fmt(thresholds.maxOriginRssKb, " KiB")}`,
    ok: !originMonitorPresent ? !requireOriginMonitor : (originReport.maxRssKb ?? Number.POSITIVE_INFINITY) <= thresholds.maxOriginRssKb
  },
  {
    name: "origin max tick p95",
    actual: originReport?.maxTickP95Us ?? null,
    threshold: `<= ${fmt(thresholds.maxOriginTickP95Us, "us")}`,
    ok: !originMonitorPresent ? !requireOriginMonitor : (originReport.maxTickP95Us ?? Number.POSITIVE_INFINITY) <= thresholds.maxOriginTickP95Us
  }
];

const passed = checks.every((check) => check.ok);
const gate = {
  generatedAt: new Date().toISOString(),
  resultDir,
  passed,
  thresholds,
  originMonitorPresent,
  checks
};

const lines = [
  "# VixArena Benchmark Gate",
  "",
  `Generated at: ${gate.generatedAt}`,
  `Result directory: \`${resultDir}\``,
  `Status: **${passed ? "passed" : "failed"}**`,
  "",
  markdownTable(["Check", "Actual", "Threshold", "Status"], checks.map((check) => [
    check.name,
    fmt(check.actual),
    check.threshold,
    passFail(check.ok)
  ])),
  ""
];

await writeFile(path.join(resultDir, "gate.json"), `${JSON.stringify(gate, null, 2)}\n`);
await writeFile(path.join(resultDir, "gate.md"), `${lines.join("\n")}\n`);

console.log(`benchmark gate ${passed ? "passed" : "failed"}: ${path.join(resultDir, "gate.md")}`);
process.exit(passed ? 0 : 1);
