# Low-latency limit order book matching engine (C++20)

A price-time-priority matching engine with a zero-allocation hot path, a lock-free SPSC ingest queue, and a
naive `std::map` reference implementation that serves as both a correctness oracle and a benchmark baseline.
Builds and runs in WSL2 (gcc 13, CMake, Ninja). No GPU needed.

> Results below are from **synthetic order flow** on one machine under WSL2 (see "Measured results" for the
> exact setup and caveats). It is not a trading system, has no network layer, and was not tested on real
> market data.

## Design

| Piece | File | Idea |
|---|---|---|
| Order book | `include/me/order_book.hpp` | levels in a flat array indexed by tick; each level an intrusive doubly-linked FIFO of orders from a preallocated pool (free list threaded through `next`); nothing allocates on the hot path |
| Best price | `include/me/bitmap.hpp` | two-level bitmap over levels; `next_set` / `prev_set` find the new best price with a couple of `ctz`/`clz` instructions when a level empties |
| Id lookup | `include/me/id_map.hpp` | open-addressing hash, linear probing, load <= 0.5, multiplicative hash, backward-shift deletion (no tombstones) |
| Ingest | `include/me/spsc_queue.hpp` | bounded SPSC ring; head and tail on separate cache lines, each side caches the other's index so the shared atomic is only re-read when the queue looks full/empty; release/acquire only, no RMW |
| Baseline | `include/me/ref_book.hpp` | `std::map<Price, std::list<Order>>` + `std::unordered_map`, same spec |
| Timing | `include/me/timing.hpp` | serialized TSC reads (`lfence` + `rdtsc`/`rdtscp`), calibrated against `steady_clock`, empty-timer floor measured and subtracted |

Semantics (shared by both books; the fuzz test enforces they agree event for event): limit orders (GTC and IOC),
cancel, and modify. Trades execute at the resting order's price. Modify that only reduces quantity at the same
price keeps time priority; any other modify is cancel/replace and loses priority (and may trade). Invalid input
is rejected with a reason (bad id, zero quantity, price out of range, duplicate id, pool full, unknown id).

## Testing

`me_tests` (23 tests, no external dependencies; also clean under AddressSanitizer + UBSan and ThreadSanitizer):

- unit tests for priority, partial fills, level exhaustion, IOC, validation, modify behavior, pool recycling
- bitmap and id-map checked against `std::set` / `std::unordered_map` under adversarial (collision-heavy) keys
- SPSC queue: 5M-element two-thread order/loss check, full/empty/wraparound edges
- **differential fuzz**: the optimized and reference books must emit identical event streams on random
  message sequences that include garbage (id 0, duplicates, zero quantity, out-of-range prices, unknown ids),
  across wide / narrow / tiny-pool / few-ids configurations, with `OrderBook::validate()` (list links, level sums,
  bitmap, best prices, id map, not crossed) run throughout. The fuzz asserts it actually exercised fills, cancels,
  modifies and each reject reason, so a pass cannot be vacuous.

Bugs the tests found while building this (kept as regression tests):

1. `IdMap::find(0)` matched an empty slot (key 0 is the empty marker) and returned a stale value, so a Cancel
   with id 0 unlinked an unrelated live order and could corrupt the lists. Found by the differential fuzz.
2. `SpscQueue` called `aligned_alloc` with a size that was not a multiple of the alignment (undefined
   behavior). Found by AddressSanitizer.

## Measured results

Setup: Intel Core Ultra 9 285K (24 cores, hybrid P+E, no SMT) under **WSL2** (Linux 6.6.87.2-microsoft-standard-WSL2),
gcc 13.3, `-O3 -march=native`, TSC 3.7 GHz. 4,000,000 messages of **synthetic** flow (seed 1): 45% passive limit
orders, 12% marketable (some IOC), 33% cancels, 10% modifies; about 2.0M fills, 1.5M rejects (cancels of orders already
filled), ~90K live orders at the end. Raw output is in `results/`. Latency is per `process()` call with serialized
TSC reads; the 12.8 ns empty-timer floor is subtracted. Optimized and reference books produced identical event
counts and fill notional in every run.

**Single thread, optimized vs textbook (`std::map` + `std::list`), `max_orders = 262144`:**

| | p50 | p90 | p99 | p99.9 | throughput |
|---|---|---|---|---|---|
| optimized | 26 ns | 121 ns | 210 ns | 307 ns | 62.8 Mmsg/s |
| reference | 43 ns | 121 ns | 295 ns | 550 ns | 22.8 Mmsg/s |

**The speedup depends on right-sizing the pool and id table to the working set.** The pool and id table are
preallocated for `max_orders`; oversizing them turns a cache-resident design into a cache-missing one:

| `max_orders` | optimized footprint | optimized p50 | optimized throughput | vs reference |
|---|---|---|---|---|
| 131,072 | 8 MB | 21 ns | 52.2 Mmsg/s | 2.4x |
| 262,144 (default, ~3x peak live) | 17 MB | 26 ns | 62.8 Mmsg/s | 2.8x |
| 1,048,576 | 67 MB | 125 ns | 28.5 Mmsg/s | 1.3x (p50 is *slower* than the reference) |

I chose the default by a rule (about 3x the peak live-order count), before looking at which number was best, and
the 1M row is shown deliberately. Throughput varies roughly 10 percent run to run on this setup.

**Queue alone** (`bench_spsc`, 32-byte messages, no matching): 420 to 500 Mmsg/s across the core pairs tried.

**Two-thread pipeline** (producer pinned to cpu 2, matching thread to cpu 4, batch 16; latency = enqueue timestamp to
end of `process()`):

| offered rate | p50 | p99 | p99.9 | max |
|---|---|---|---|---|
| 250K msg/s | 228 ns | 195 us | 574 us | 1.2 ms |
| 500K msg/s | 177 ns | 53 us | 202 us | 0.66 ms |
| 1M msg/s | 175 ns | 160 us | 234 us | 0.48 ms |
| 2M msg/s | 169 ns | 172 us | 214 us | 0.37 ms |

Unthrottled sustained throughput (5 runs each): batch 1: 8 to 23 Mmsg/s (very noisy); batch 16: 21 to 25;
batch 64: 23 to 28. The book alone does 52 to 63 Mmsg/s, so cross-core transfer still costs roughly half.

### What these numbers do and do not support

- **The p50 of about 170 to 230 ns through the pipeline is the credible part** (queue transfer plus matching). The
  **p99 and above in the pipeline (tens to hundreds of microseconds) are not engine latency.** The single-thread
  p99.9 is about 300 ns on the same machine, so those tails are WSL2/hypervisor scheduling jitter. Trustworthy
  cross-thread tail numbers need bare-metal Linux with isolated cores (`isolcpus`, `nohz_full`) and a verified
  P-core pinning; WSL2 hides the real topology, so "cpu 2 and cpu 4" may not be P-cores.
- The single-thread `max` column (0.2 to 0.9 ms) is likewise OS/hypervisor noise.
- The flow is synthetic with a random-walk mid price; the book depth, fill rate and cancel behavior are
  assumptions, not market data. The reference book is the textbook design, not a tuned competitor, so say
  "faster than a std::map/std::list book", never "faster than" any real exchange or library.
- Cross-thread latency assumes a synchronized, invariant TSC across cores.
- `scripts/profile.sh` (perf, then cachegrind fallback) is written but has **not been run**.

### Findings from building it

1. **Cache footprint dominated everything**: sizing alone moved the optimized book from slower-than-baseline to 2.8x.
2. **A tight pop loop hid cross-core latency; a real consumer exposed it.** The queue alone sustains hundreds of
   millions of messages per second, but with engine work between pops the pipeline fell to 7 to 10 Mmsg/s because
   each cache-line transfer was exposed. Batch popping (`try_pop_batch`) roughly tripled it.

## Not done / next steps

- Profile with perf or cachegrind and attribute the remaining cost (id-table misses on cancel, pool locality).
- Prefetch the next order and level in the match loop; try 12-byte id-map slots.
- Measure on bare-metal Linux with isolated, verified P-cores to get real cross-thread tails.
- Replay a real or realistic L2 feed, add a network/serialization edge, add self-trade prevention, market orders,
  and price-band checks.

## Run

```bash
bash scripts/build.sh                  # ~/me-build/release (WSL)
~/me-build/release/me_tests            # tests (scripts/run_tests.sh runs each group with a timeout)
bash scripts/sanitize.sh               # ASan+UBSan and TSan builds of the tests
bash scripts/run_bench.sh              # all benchmarks -> results/ (quiet machine; never alongside sanitizers)
bash scripts/sweep_capacity.sh         # capacity sensitivity
bash scripts/diag_pipeline.sh          # pipeline cost breakdown / batch sizes
bash scripts/profile.sh                # perf if available, else cachegrind (not yet run)
```

From Windows: `wsl -d Ubuntu-24.04 -u root -- bash /mnt/c/.../04-cpp-matching-engine/scripts/build.sh`.

## Phrasing rules (for the resume)

- "Synthetic order flow", "WSL2", and the capacity dependence must travel with any number. Safe form: "price-time
  priority matching engine in C++20 with a lock-free SPSC ingest queue; 2.8x the throughput and about half the p99.9
  latency of a std::map/std::list baseline on synthetic flow (single thread, WSL2)".
- Do not quote the pipeline p99 or max as engine latency. Do not claim production use, exchange-grade latency, or
  results on real market data.
- Differential fuzz against a reference book plus ASan/UBSan/TSan is a real, citable part of the engineering.
