// Two-thread pipeline: producer -> SPSC queue -> matching thread.
//   ./bench_pipeline [--n 2000000] [--prod-cpu 2] [--cons-cpu 4] [--rates 500000,1000000,2000000,0]
// The producer stamps each message with the TSC at enqueue; the consumer records (TSC after process() minus
// stamp), so latency = queueing delay + matching time. A rate of 0 means unthrottled: the queue saturates, so
// it measures sustained throughput and the latency column is queue-wait dominated (not meaningful).
// Assumes an invariant, cross-core-synchronized TSC (true on modern x86; also true under WSL2 in practice).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>

#include "me/flow.hpp"
#include "me/order_book.hpp"
#include "me/spsc_queue.hpp"
#include "me/timing.hpp"

using namespace me;

struct CountSink {
  std::uint64_t events = 0, fills = 0;
  void operator()(const Event& e) { ++events; fills += e.type == EvType::Fill; }
};

static bool g_no_engine = false;  // --no-engine: consumer only pops and timestamps (isolates queue + timing cost)
static std::size_t g_batch = 16;  // --batch N: max messages the consumer drains per pop (1 = one at a time)

static void run_rate(const std::vector<Msg>& msgs, double rate, int prod_cpu, int cons_cpu) {
  static SpscQueue<Msg, 1 << 16> q;
  const std::size_t n = msgs.size();
  std::vector<std::uint32_t> lat(n);
  std::atomic<bool> go{false};
  const std::uint64_t gap = rate > 0 ? static_cast<std::uint64_t>(tsc_ghz() * 1e9 / rate) : 0;

  std::thread prod([&] {
    if (prod_cpu >= 0) pin_to_cpu(prod_cpu);
    while (!go.load(std::memory_order_acquire)) {}
    std::uint64_t next = rdtsc();
    for (std::size_t i = 0; i < n; ++i) {
      if (gap) { next += gap; while (rdtsc() < next) {} }
      Msg m = msgs[i];
      m.ts = rdtsc();
      while (!q.try_push(m)) {}
    }
  });

  OrderBook<CountSink> book(BookConfig{0, 65536, 1u << 18});  // ~3x the peak live orders; see README capacity sweep
  if (cons_cpu >= 0) pin_to_cpu(cons_cpu);
  auto t0 = std::chrono::steady_clock::now();
  go.store(true, std::memory_order_release);
  Msg buf[64];
  const std::size_t bmax = std::min<std::size_t>(g_batch, 64);
  for (std::size_t i = 0; i < n;) {
    std::size_t k = q.try_pop_batch(buf, std::min(bmax, n - i));
    for (std::size_t j = 0; j < k; ++j) {
      if (!g_no_engine) book.process(buf[j]);
      lat[i++] = static_cast<std::uint32_t>(rdtsc() - buf[j].ts);
    }
  }
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  prod.join();

  auto p = percentiles(lat);
  std::printf("| %9s | %9.2f | %7.0f | %7.0f | %8.0f | %9.0f | %9.0f |\n",
              rate > 0 ? std::to_string(static_cast<long long>(rate)).c_str() : "max", static_cast<double>(n) / secs / 1e6,
              p.p50, p.p99, p.p999, p.max, p.mean);
}

int main(int argc, char** argv) {
  FlowConfig fc;
  fc.n = 2'000'000;
  int prod_cpu = 2, cons_cpu = 4;
  std::string rates = "250000,500000,1000000,2000000,0";
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--n") && i + 1 < argc) fc.n = std::strtoull(argv[++i], nullptr, 10);
    else if (!std::strcmp(argv[i], "--prod-cpu") && i + 1 < argc) prod_cpu = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--cons-cpu") && i + 1 < argc) cons_cpu = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--rates") && i + 1 < argc) rates = argv[++i];
    else if (!std::strcmp(argv[i], "--no-engine")) g_no_engine = true;
    else if (!std::strcmp(argv[i], "--batch") && i + 1 < argc) g_batch = std::strtoull(argv[++i], nullptr, 10);
  }
  auto msgs = generate_flow(fc);
  std::printf("TSC %.3f GHz, producer cpu %d, matching cpu %d, %zu msgs per run\n\n", tsc_ghz(), prod_cpu, cons_cpu, fc.n);
  std::printf("| rate msg/s | Mmsg/s    | p50 ns  | p99 ns  | p99.9 ns | max ns    | mean ns   |\n");
  std::printf("|------------|-----------|---------|---------|----------|-----------|-----------|\n");
  std::stringstream ss(rates);
  for (std::string tok; std::getline(ss, tok, ',');) run_rate(msgs, std::atof(tok.c_str()), prod_cpu, cons_cpu);
  return 0;
}
