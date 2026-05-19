#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

fail() {
  echo "benchmark tooling check failed: $*" >&2
  exit 1
}

for script in \
  scripts/benchmark_report.mjs \
  scripts/benchmark_compare.mjs \
  scripts/benchmark_gate.mjs \
  scripts/benchmark_monitor_report.mjs \
  scripts/ws_load_test.mjs
do
  node --check "${script}" >/dev/null
done

bash -n \
  scripts/benchmark_suite.sh \
  scripts/benchmark_extreme.sh \
  scripts/benchmark_one_step.sh \
  scripts/benchmark_origin_monitor.sh \
  scripts/benchmark_profile.sh

work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

baseline_dir="${work_dir}/baseline"
candidate_dir="${work_dir}/candidate"
fail_dir="${work_dir}/fail"
mkdir -p "${baseline_dir}" "${candidate_dir}" "${fail_dir}"

write_fixture_run() {
  local dir="$1"
  local rps="$2"
  local ws_p95="$3"

  cat >"${dir}/root.log" <<EOF
Running 30s test @ https://vix.micutu.com/
  8 threads and 300 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    34.27ms    6.38ms 168.15ms   88.58%
    Req/Sec     1.09k    87.65     1.23k    93.79%
  518687 requests in 30.00s, 4.18GB read
Requests/sec:   ${rps}
Transfer/sec:     71.28MB
EOF

  cat >"${dir}/ws-sharded.log" <<EOF
{
  "clients": 200,
  "room": "load-check",
  "rooms": 4,
  "durationMs": 30000,
  "welcomed": 200,
  "welcomedCount": 200,
  "connectAttempts": 200,
  "snapshots": 64000,
  "pongs": 3000,
  "closes": 200,
  "serverErrors": 0,
  "expectedDefensiveErrors": 0,
  "unexpectedServerErrors": 0,
  "serverErrorsByMessage": {},
  "bytes": 1234567,
  "latencyMs": {
    "p50": 48,
    "p95": ${ws_p95},
    "max": 180
  }
}
load test ok
EOF

  cat >"${dir}/stats-before.json" <<'EOF'
{
  "totalConnectionsSinceStart": 100,
  "totalChatMessagesSinceStart": 10,
  "websocket": {
    "messagesReceived": 1000,
    "messagesSent": 2000,
    "rejectedMessages": 4,
    "rateLimitRejects": 8,
    "rejectedConnections": 0,
    "protocolViolations": 0,
    "sendFailures": 0,
    "snapshotsSent": 500,
    "snapshotDeltasSent": 600,
    "snapshotBytesSent": 10000,
    "snapshotDeltaBytesSent": 20000
  }
}
EOF

  cat >"${dir}/stats-after.json" <<'EOF'
{
  "totalConnectionsSinceStart": 300,
  "totalChatMessagesSinceStart": 10,
  "websocket": {
    "messagesReceived": 3000,
    "messagesSent": 6000,
    "rejectedMessages": 4,
    "rateLimitRejects": 8,
    "rejectedConnections": 0,
    "protocolViolations": 0,
    "sendFailures": 0,
    "snapshotsSent": 1500,
    "snapshotDeltasSent": 1600,
    "snapshotBytesSent": 30000,
    "snapshotDeltaBytesSent": 80000
  }
}
EOF
}

write_fixture_run "${baseline_dir}" "8000.00" "140"
write_fixture_run "${candidate_dir}" "9000.00" "120"
write_fixture_run "${fail_dir}" "4000.00" "450"

node scripts/benchmark_report.mjs "${baseline_dir}" >/dev/null
node scripts/benchmark_report.mjs "${candidate_dir}" >/dev/null
node scripts/benchmark_report.mjs "${fail_dir}" >/dev/null

node - <<'NODE' "${candidate_dir}/report.json"
const fs = require("node:fs");
const report = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
const http = report.results.find((item) => item.kind === "wrk");
const ws = report.results.find((item) => item.kind === "websocket");
if (!http || http.requestsPerSecond !== 9000) process.exit(1);
if (!ws || ws.welcomed !== 200 || ws.clients !== 200 || ws.latencyMs.p95 !== 120) process.exit(1);
if (report.statsDelta["websocket.protocolViolations"] !== 0) process.exit(1);
NODE

cat >"${candidate_dir}/origin-monitor.jsonl" <<'EOF'
{"timestamp":"2026-05-19T00:00:00Z","rssKb":50000,"cpuPercent":20,"memPercent":1.1,"threads":15,"appStats":{"connectedPlayers":20,"tickDurationUs":{"p95":900},"totalConnectionsSinceStart":100,"totalChatMessagesSinceStart":5,"websocket":{"messagesReceived":1000,"messagesSent":2000,"rejectedMessages":0,"rateLimitRejects":0,"protocolViolations":0,"sendFailures":0,"snapshotsSent":100,"snapshotDeltasSent":200}}}
{"timestamp":"2026-05-19T00:00:01Z","rssKb":52000,"cpuPercent":35,"memPercent":1.2,"threads":16,"appStats":{"connectedPlayers":40,"tickDurationUs":{"p95":1200},"totalConnectionsSinceStart":200,"totalChatMessagesSinceStart":8,"websocket":{"messagesReceived":3000,"messagesSent":6000,"rejectedMessages":0,"rateLimitRejects":0,"protocolViolations":0,"sendFailures":0,"snapshotsSent":400,"snapshotDeltasSent":700}}}
EOF

node scripts/benchmark_monitor_report.mjs "${candidate_dir}/origin-monitor.jsonl" "${candidate_dir}" >/dev/null

BENCH_GATE_REQUIRE_ORIGIN_MONITOR=1 \
BENCH_GATE_MIN_HTTP_RPS=8000 \
BENCH_GATE_MIN_WS_CLIENTS=200 \
BENCH_GATE_MAX_WS_P95_MS=200 \
BENCH_GATE_MAX_UNEXPECTED_SERVER_ERRORS=0 \
BENCH_GATE_MAX_PROTOCOL_VIOLATIONS_DELTA=0 \
BENCH_GATE_MAX_REJECTED_CONNECTIONS_DELTA=0 \
BENCH_GATE_MAX_SEND_FAILURES_DELTA=0 \
BENCH_GATE_ORIGIN_MIN_SAMPLES=2 \
BENCH_GATE_ORIGIN_MAX_CPU_PERCENT=80 \
BENCH_GATE_ORIGIN_MAX_RSS_KB=100000 \
BENCH_GATE_ORIGIN_MAX_TICK_P95_US=2000 \
node scripts/benchmark_gate.mjs "${candidate_dir}" >/dev/null

grep -Fq 'Status: **passed**' "${candidate_dir}/gate.md" || fail "passing gate report was not marked passed"

if BENCH_GATE_REQUIRE_ORIGIN_MONITOR=1 \
  BENCH_GATE_MIN_HTTP_RPS=8000 \
  BENCH_GATE_MIN_WS_CLIENTS=200 \
  BENCH_GATE_MAX_WS_P95_MS=200 \
  node scripts/benchmark_gate.mjs "${fail_dir}" >/dev/null 2>&1
then
  fail "failing gate fixture unexpectedly passed"
fi

grep -Fq 'Status: **failed**' "${fail_dir}/gate.md" || fail "failing gate report was not marked failed"

node scripts/benchmark_compare.mjs "${baseline_dir}" "${candidate_dir}" >/dev/null
grep -Fq 'Best HTTP RPS: 8,000 -> 9,000' "${candidate_dir}/comparison.md" || fail "comparison summary missing expected HTTP delta"

echo "benchmark tooling check ok"
