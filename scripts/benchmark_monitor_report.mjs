import { readFile, writeFile } from "node:fs/promises";
import path from "node:path";

const input = process.argv[2];
const outDir = process.argv[3] || (input ? path.dirname(input) : "");

if (!input) {
  console.error("usage: node scripts/benchmark_monitor_report.mjs <origin-monitor.jsonl> [out-dir]");
  process.exit(2);
}

function fmt(value, suffix = "") {
  if (value === null || value === undefined || Number.isNaN(value)) return "-";
  if (typeof value === "number") {
    return `${new Intl.NumberFormat("en-US", { maximumFractionDigits: 2 }).format(value)}${suffix}`;
  }
  return `${value}${suffix}`;
}

function max(values) {
  const nums = values.filter((value) => typeof value === "number" && Number.isFinite(value));
  return nums.length ? Math.max(...nums) : null;
}

function last(values) {
  return values.length ? values[values.length - 1] : null;
}

const lines = (await readFile(input, "utf8")).split(/\r?\n/).filter(Boolean);
const samples = [];
for (const line of lines) {
  try {
    samples.push(JSON.parse(line));
  } catch {}
}

const firstSample = samples[0] || null;
const lastSample = last(samples);
const firstStats = firstSample?.appStats || {};
const lastStats = lastSample?.appStats || {};
const delta = {};
for (const [label, pathParts] of Object.entries({
  totalConnectionsSinceStart: ["totalConnectionsSinceStart"],
  totalChatMessagesSinceStart: ["totalChatMessagesSinceStart"],
  messagesReceived: ["websocket", "messagesReceived"],
  messagesSent: ["websocket", "messagesSent"],
  rejectedMessages: ["websocket", "rejectedMessages"],
  rateLimitRejects: ["websocket", "rateLimitRejects"],
  protocolViolations: ["websocket", "protocolViolations"],
  sendFailures: ["websocket", "sendFailures"],
  snapshotsSent: ["websocket", "snapshotsSent"],
  snapshotDeltasSent: ["websocket", "snapshotDeltasSent"]
})) {
  const get = (obj) => pathParts.reduce((current, part) => current?.[part], obj);
  const before = get(firstStats);
  const after = get(lastStats);
  if (typeof before === "number" && typeof after === "number") {
    delta[label] = after - before;
  }
}

const report = {
  generatedAt: new Date().toISOString(),
  input,
  samples: samples.length,
  startedAt: firstSample?.timestamp || null,
  endedAt: lastSample?.timestamp || null,
  maxRssKb: max(samples.map((sample) => sample.rssKb)),
  maxCpuPercent: max(samples.map((sample) => sample.cpuPercent)),
  maxMemPercent: max(samples.map((sample) => sample.memPercent)),
  maxThreads: max(samples.map((sample) => sample.threads)),
  maxTickP95Us: max(samples.map((sample) => sample.appStats?.tickDurationUs?.p95)),
  maxPlayers: max(samples.map((sample) => sample.appStats?.connectedPlayers)),
  counterDelta: delta
};

const rows = [
  ["samples", fmt(report.samples)],
  ["started", fmt(report.startedAt)],
  ["ended", fmt(report.endedAt)],
  ["max RSS", fmt(report.maxRssKb, " KiB")],
  ["max CPU", fmt(report.maxCpuPercent, "%")],
  ["max memory", fmt(report.maxMemPercent, "%")],
  ["max threads", fmt(report.maxThreads)],
  ["max tick p95", fmt(report.maxTickP95Us, "us")],
  ["max connected players", fmt(report.maxPlayers)]
];

const deltaRows = Object.entries(report.counterDelta).map(([key, value]) => [key, fmt(value)]);
const table = (headers, rowsToRender) => [
  `| ${headers.join(" | ")} |`,
  `| ${headers.map(() => "---").join(" | ")} |`,
  ...rowsToRender.map((row) => `| ${row.join(" | ")} |`)
].join("\n");

const markdown = [
  "# VixArena Origin Monitor Report",
  "",
  `Generated at: ${report.generatedAt}`,
  `Input: \`${input}\``,
  "",
  "## Resource Summary",
  "",
  table(["Metric", "Value"], rows),
  "",
  "## Runtime Counter Delta",
  "",
  deltaRows.length ? table(["Metric", "Delta"], deltaRows) : "_No counter delta available._",
  ""
].join("\n");

await writeFile(path.join(outDir, "origin-monitor-report.json"), `${JSON.stringify(report, null, 2)}\n`);
await writeFile(path.join(outDir, "origin-monitor-report.md"), `${markdown}\n`);
console.log(`origin monitor report written: ${path.join(outDir, "origin-monitor-report.md")}`);
