# VixArena Benchmark Notes

This document records repeatable benchmark commands and the latest measured results for VixArena. Results are environment-dependent and should be treated as a snapshot, not a permanent capacity guarantee.

## Test Environment

- App: VixArena C++20 Boost.Beast HTTP/WebSocket server
- Public URL: `https://vix.micutu.com`
- Origin binding: `127.0.0.1:18080` behind Nginx
- Benchmark path: external benchmark VPS to Vix VPS origin
- DNS mode used for the reported origin tests: benchmark VPS mapped `vix.micutu.com` directly to the origin IP with `/etc/hosts`, bypassing Cloudflare while still using HTTPS/Nginx
- Benchmark source IP: `81.181.166.237`

## Safety Profile

Production defaults keep strict per-IP and message limits. High-load testing should use the temporary benchmark allowlist profile only for trusted source IPs.

Enable on the Vix VPS:

```bash
cd /home/micu/vix

BENCHMARK_MAX_CONNECTIONS_PER_IP=1024 \
BENCHMARK_MAX_PLAYERS_PER_ROOM=512 \
BENCHMARK_WS_MESSAGE_BURST=1200 \
BENCHMARK_WS_MESSAGE_REFILL_PER_SECOND=600 \
BENCHMARK_HTTP_RATE_LIMIT_BURST=50000 \
BENCHMARK_HTTP_RATE_LIMIT_REFILL_PER_SECOND=30000 \
scripts/benchmark_profile.sh enable 81.181.166.237
```

Disable after testing:

```bash
cd /home/micu/vix
scripts/benchmark_profile.sh disable
```

## Standard Benchmark

Run from the benchmark VPS:

```bash
cd ~/vix

TARGET=https://vix.micutu.com \
ORIGIN_IP=57.129.112.224 \
DURATION_SECONDS=60 \
WS_CLIENTS_HIGH=200 \
WS_ROOMS_HIGH=4 \
scripts/benchmark_suite.sh
```

The suite writes raw logs plus `stats-before.json`, `stats-after.json`, `summary.txt`, `report.json`, and `report.md` into `benchmark-results/<timestamp>/`. Reports are generated even when a benchmark phase fails, so partial results remain inspectable.

Latest standard result:

| Test | Result |
| --- | --- |
| HTTP `/` | 8,636 RPS at 300 concurrent connections |
| HTTP `/api/state` | 5,926 RPS at 300 concurrent connections |
| HTTP `/api/stats` | 7,181 RPS at 300 concurrent connections |
| WebSocket sharded | 200/200 clients welcomed across 4 rooms |
| WebSocket p95 latency | 225 ms under the 200-client public-network load test |

## Extreme Benchmark

Run from the benchmark VPS:

```bash
cd ~/vix

TARGET=https://vix.micutu.com \
ORIGIN_IP=57.129.112.224 \
DURATION_SECONDS=90 \
WRK_SPIKE_CONNECTIONS=600 \
WS_SHARDED_CLIENTS=500 \
WS_SHARDED_ROOMS=10 \
scripts/benchmark_extreme.sh
```

The extreme suite also writes raw logs plus `stats-before.json`, `stats-after.json`, `summary.txt`, `report.json`, and `report.md` into `benchmark-results/extreme-<timestamp>/`. Reports include HTTP/WS summaries and runtime counter deltas from `/api/stats`.

## Comparing Runs

After two benchmark runs have generated `report.json`, compare them with:

```bash
node scripts/benchmark_compare.mjs \
  benchmark-results/<baseline-dir> \
  benchmark-results/<candidate-dir>
```

The comparator writes `comparison.md` and `comparison.json` into the candidate directory. It compares best HTTP throughput, largest WebSocket run, WebSocket p95 latency, and matching test names across both reports.

## Performance Gate

After a benchmark has generated `report.json`, validate it against explicit thresholds:

```bash
BENCH_GATE_MIN_HTTP_RPS=8000 \
BENCH_GATE_MIN_WS_CLIENTS=200 \
BENCH_GATE_MAX_WS_P95_MS=300 \
BENCH_GATE_MAX_UNEXPECTED_SERVER_ERRORS=0 \
BENCH_GATE_MAX_PROTOCOL_VIOLATIONS_DELTA=0 \
BENCH_GATE_MAX_SEND_FAILURES_DELTA=0 \
BENCH_GATE_ORIGIN_MAX_CPU_PERCENT=90 \
BENCH_GATE_ORIGIN_MAX_RSS_KB=262144 \
BENCH_GATE_ORIGIN_MAX_TICK_P95_US=5000 \
node scripts/benchmark_gate.mjs benchmark-results/<result-dir>
```

The gate writes `gate.md` and `gate.json` into the benchmark result directory and exits non-zero if any threshold fails. Origin thresholds are only enforced when `origin-monitor-report.json` exists, unless `BENCH_GATE_REQUIRE_ORIGIN_MONITOR=1` is set.

The one-step benchmark helper can run the gate automatically:

```bash
SOURCE_IP=81.181.166.237 \
ORIGIN_SSH=micu@57.129.112.224 \
TARGET=https://vix.micutu.com \
RUN_BENCHMARK_GATE=true \
BENCH_GATE_MIN_HTTP_RPS=8000 \
BENCH_GATE_MIN_WS_CLIENTS=200 \
BENCH_GATE_MAX_WS_P95_MS=300 \
scripts/benchmark_one_step.sh
```

For the full extreme suite, add `BENCHMARK_SUITE=extreme`:

```bash
SOURCE_IP=81.181.166.237 \
ORIGIN_SSH=micu@57.129.112.224 \
TARGET=https://vix.micutu.com \
BENCHMARK_SUITE=extreme \
RUN_BENCHMARK_GATE=true \
BENCH_GATE_MIN_HTTP_RPS=12000 \
BENCH_GATE_MIN_WS_CLIENTS=500 \
BENCH_GATE_MAX_WS_P95_MS=350 \
BENCH_GATE_REQUIRE_ORIGIN_MONITOR=1 \
BENCH_GATE_ORIGIN_MAX_CPU_PERCENT=90 \
BENCH_GATE_ORIGIN_MAX_RSS_KB=262144 \
BENCH_GATE_ORIGIN_MAX_TICK_P95_US=5000 \
scripts/benchmark_one_step.sh
```

To collect origin CPU/memory/service telemetry during the run, add `RUN_ORIGIN_MONITOR=true`:

```bash
SOURCE_IP=81.181.166.237 \
ORIGIN_SSH=micu@57.129.112.224 \
TARGET=https://vix.micutu.com \
BENCHMARK_SUITE=extreme \
RUN_ORIGIN_MONITOR=true \
RUN_BENCHMARK_GATE=true \
BENCH_GATE_MIN_HTTP_RPS=12000 \
BENCH_GATE_MIN_WS_CLIENTS=500 \
BENCH_GATE_MAX_WS_P95_MS=350 \
scripts/benchmark_one_step.sh
```

This copies `origin-monitor.jsonl`, `origin-monitor-report.md`, and `origin-monitor-report.json` into the benchmark result directory.

The extreme script currently runs:

- HTTP spike tests for `/`, `/api/state`, and `/api/stats`
- dense single-room WebSocket load with input, ping, chat, and abilities
- sharded WebSocket load with reconnect churn
- mixed HTTP plus WebSocket contention

Latest extreme partial result:

| Test | Result |
| --- | --- |
| HTTP `/` | 16,172 RPS at 600 concurrent connections |
| HTTP `/api/state` | 7,031 RPS at 600 concurrent connections |
| Dense WebSocket room | 256/256 clients welcomed in one room |
| Dense WebSocket p95 latency | 131 ms |
| Dense WebSocket max latency | 1,960 ms |
| Protocol violations | 0 |
| Rejected connections | 0 |
| Send failures | 0 |
| Rate-limit rejects | 3,448 |
| Rejected messages | 1,254 |

The original extreme run stopped during the dense WebSocket phase because the load client treated all server `error` messages as failures. Those errors were defensive rejections from rate limiting and gameplay cooldowns under intentionally aggressive input/chat/ability traffic. They were not protocol parser failures, connection rejections, send failures, or server crashes.

`scripts/ws_load_test.mjs` now supports `VIX_LOAD_EXPECT_DEFENSIVE_ERRORS=true`, which reports known defensive errors separately:

- `message rate limit`
- `chat rate limit`
- `dash cooldown`
- `shield cooldown`
- `magnet cooldown`

With that mode enabled, the extreme benchmark still fails on unexpected server errors, protocol/client errors, missing welcomes, or missing snapshots, but it can continue through defensive rate-limit/cooldown behavior.

## Interpretation

The strongest current claim is:

```text
Benchmarked at 16k+ HTTP RPS and 256 concurrent WebSocket clients in a dense single-room stress test, with 0 protocol violations, 0 rejected connections, and p95 WebSocket latency around 131 ms under defensive rate limiting.
```

For a cleaner 500-client WebSocket claim, rerun the current extreme script and report `expectedDefensiveErrors` separately from `unexpectedServerErrors`.

## CV-Grade Benchmark Gate

For a result worth publishing in a portfolio or CV, prefer a run that includes:

- direct-origin HTTP and WebSocket tests
- `RUN_ORIGIN_MONITOR=true`
- `RUN_BENCHMARK_GATE=true`
- `welcomed == clients` for every WebSocket phase
- `unexpectedServerErrors == 0`
- `protocolViolations == 0`
- `rejectedConnections == 0`
- `sendFailures == 0`
- origin CPU, RSS, and tick p95 under explicit thresholds

Suggested gate for a strong public claim:

```bash
SOURCE_IP=81.181.166.237 \
ORIGIN_SSH=micu@57.129.112.224 \
TARGET=https://vix.micutu.com \
BENCHMARK_SUITE=extreme \
RUN_ORIGIN_MONITOR=true \
RUN_BENCHMARK_GATE=true \
BENCH_GATE_REQUIRE_ORIGIN_MONITOR=1 \
BENCH_GATE_MIN_HTTP_RPS=12000 \
BENCH_GATE_MIN_WS_CLIENTS=500 \
BENCH_GATE_MAX_WS_P95_MS=350 \
BENCH_GATE_MAX_UNEXPECTED_SERVER_ERRORS=0 \
BENCH_GATE_MAX_PROTOCOL_VIOLATIONS_DELTA=0 \
BENCH_GATE_MAX_REJECTED_CONNECTIONS_DELTA=0 \
BENCH_GATE_MAX_SEND_FAILURES_DELTA=0 \
BENCH_GATE_ORIGIN_MIN_SAMPLES=10 \
BENCH_GATE_ORIGIN_MAX_CPU_PERCENT=90 \
BENCH_GATE_ORIGIN_MAX_RSS_KB=262144 \
BENCH_GATE_ORIGIN_MAX_TICK_P95_US=5000 \
scripts/benchmark_one_step.sh
```

If this passes, keep the generated `report.md`, `gate.md`, `comparison.md` if available, and `origin-monitor-report.md` as the evidence bundle for the claim.

## What To Watch

During high-load tests, monitor:

```bash
curl -s http://127.0.0.1:18080/api/stats | jq '.tickDurationUs, .websocket'
systemctl status vix-arena.service --no-pager
journalctl -u vix-arena.service -n 100 --no-pager
```

Key metrics:

- HTTP RPS and p95/p99 latency
- WebSocket `welcomed == clients`
- WebSocket p95 latency
- `protocolViolations`
- `rejectedConnections`
- `sendFailures`
- `rateLimitRejects`
- `rejectedMessages`
- tick duration under load
- service memory and CPU

## Snapshot Serialization Scaling

Concurrent live players (persistent WebSocket sessions serialized every tick)
are a different axis from HTTP RPS. The 20Hz tick has a 50ms/tick budget;
past it the delivered snapshot rate collapses.

Two changes moved the concurrent-player ceiling substantially (measured on the
origin box with a co-located load generator, so conservative — a dedicated
generator would read higher). Metric is server-reported tick p95 under light
input across sharded rooms:

| Concurrent players | Baseline | + delta memoization & shared baseline | + parallel per-room serialization |
| ---: | ---: | ---: | ---: |
| 150 | 125ms | 16ms | — |
| 500 | stalls | 43ms | 23ms |
| 1000 | stalls | stalls | 47ms (under budget) |

- **Delta memoization + shared baseline:** every client in a room that
  acknowledged the same snapshot shares one baseline, so the delta is computed
  and serialized once per baseline instead of once per client, and baselines
  advance by pointer copy instead of a deep JSON copy.
- **Parallel per-room serialization:** rooms are independent, so the full dump
  and deltas are built off the game mutex across a worker pool. The game mutex
  is held only for simulation and snapshot construction, so joins/inputs/chat
  are no longer blocked by serialization. Verified race-free under
  ThreadSanitizer with a mixed chat/ability/reconnect multi-room load.

## Notes

- Benchmark profile is intentionally temporary and should not be left enabled after tests.
- Cloudflare can change public-edge benchmark results. Direct-origin tests are more useful for measuring the app and Nginx.
- WebSocket single-room tests are much heavier than sharded-room tests because every snapshot fanout is concentrated in one room.
- Chat and ability-heavy tests intentionally trigger cooldown and rate-limit behavior.
