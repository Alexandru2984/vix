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

## Notes

- Benchmark profile is intentionally temporary and should not be left enabled after tests.
- Cloudflare can change public-edge benchmark results. Direct-origin tests are more useful for measuring the app and Nginx.
- WebSocket single-room tests are much heavier than sharded-room tests because every snapshot fanout is concentrated in one room.
- Chat and ability-heavy tests intentionally trigger cooldown and rate-limit behavior.
