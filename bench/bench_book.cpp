// Single-threaded book benchmark: optimized OrderBook vs textbook RefBook on identical synthetic flow.
//   ./bench_book [--n 4000000] [--seed 1] [--cpu 2]
// Throughput: whole-run wall time, best of 3 (no per-op timers in the loop).
// Latency:    separate pass, each process() call timed with serialized TSC reads; the empty-timer floor
//             (median cost of two back-to-back timestamps) is subtracted and printed.
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "me/flow.hpp"
#include "me/order_book.hpp"
#include "me/ref_book.hpp"
#include "me/timing.hpp"

using namespace me;

struct CountSink {
  std::uint64_t events = 0, fills = 0, notional = 0, cancels = 0, rejects = 0, filled_qty = 0;
  void operator()(const Event& e) {
    ++events;
    if (e.type == EvType::Fill) { ++fills; notional += static_cast<std::uint64_t>(e.price) * e.qty; filled_qty += e.qty; }
    else if (e.type == EvType::Cancelled) ++cancels;
    else if (e.type == EvType::Rejected) ++rejects;
  }
};

static void print_row(const char* impl, const char* what, const Pcts& p, std::size_t n) {
  std::printf("| %-9s | %-8s | %8zu | %7.0f | %7.0f | %7.0f | %8.0f | %9.0f | %7.0f |\n", impl, what, n, p.p50, p.p90,
              p.p99, p.p999, p.max, p.mean);
}

template <class Book>
CountSink run(const char* impl, const std::vector<Msg>& msgs, const BookConfig& cfg, std::uint64_t floor_cycles,
              double* mops_out, std::size_t* live_out) {
  // throughput
  double best = 0;
  for (int rep = 0; rep < 3; ++rep) {
    Book b(cfg);
    auto t0 = std::chrono::steady_clock::now();
    for (const Msg& m : msgs) b.process(m);
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    best = std::max(best, static_cast<double>(msgs.size()) / s / 1e6);
  }
  *mops_out = best;

  // latency
  Book b(cfg);
  std::vector<std::uint32_t> all, nw, cx, md;
  all.reserve(msgs.size()); nw.reserve(msgs.size()); cx.reserve(msgs.size() / 2); md.reserve(msgs.size() / 8);
  for (const Msg& m : msgs) {
    std::uint64_t a = tsc_begin();
    b.process(m);
    std::uint64_t e = tsc_end();
    auto c = static_cast<std::uint32_t>(e - a);
    all.push_back(c);
    (m.type == MsgType::Cancel ? cx : m.type == MsgType::Modify ? md : nw).push_back(c);
  }
  for (auto [name, v] : {std::pair<const char*, std::vector<std::uint32_t>*>{"all", &all}, {"new", &nw}, {"cancel", &cx}, {"modify", &md}}) {
    std::size_t n = v->size();
    print_row(impl, name, percentiles(*v, floor_cycles), n);
  }
  *live_out = b.live_orders();
  return b.sink();
}

int main(int argc, char** argv) {
  FlowConfig fc;
  int cpu = -1;
  std::uint32_t max_orders = 1u << 18;  // ~3x the ~90K peak live orders in this flow; see README capacity sweep
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--max-orders") && i + 1 < argc) max_orders = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    else if (!std::strcmp(argv[i], "--n") && i + 1 < argc) fc.n = std::strtoull(argv[++i], nullptr, 10);
    else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) fc.seed = std::strtoull(argv[++i], nullptr, 10);
    else if (!std::strcmp(argv[i], "--cpu") && i + 1 < argc) cpu = std::atoi(argv[++i]);
  }
  if (cpu >= 0 && !pin_to_cpu(cpu)) std::fprintf(stderr, "warning: could not pin to cpu %d\n", cpu);

  std::printf("generating %zu messages (seed %llu)...\n", fc.n, static_cast<unsigned long long>(fc.seed));
  auto msgs = generate_flow(fc);
  BookConfig cfg{0, 65536, max_orders};
  std::printf("max_orders %u: pool %.1f MB + id table %.1f MB (optimized book)\n", max_orders,
              (max_orders + 1) * 32.0 / 1e6, std::bit_ceil(std::max<std::size_t>(max_orders * 2ull, 16)) * 16.0 / 1e6);
  const double ghz = tsc_ghz();
  const std::uint64_t floor = timer_floor_cycles();
  std::printf("TSC %.3f GHz, timer floor %llu cycles (%.1f ns), subtracted below\n\n", ghz,
              static_cast<unsigned long long>(floor), static_cast<double>(floor) / ghz);

  std::printf("| impl      | ops      | count    | p50 ns  | p90 ns  | p99 ns  | p99.9 ns | max ns    | mean ns |\n");
  std::printf("|-----------|----------|----------|---------|---------|---------|----------|-----------|---------|\n");
  double mops_fast = 0, mops_ref = 0;
  std::size_t live_fast = 0, live_ref = 0;
  CountSink sf = run<OrderBook<CountSink>>("optimized", msgs, cfg, floor, &mops_fast, &live_fast);
  CountSink sr = run<RefBook<CountSink>>("reference", msgs, cfg, floor, &mops_ref, &live_ref);

  std::printf("\nthroughput (best of 3): optimized %.2f Mmsg/s, reference %.2f Mmsg/s (%.1fx)\n", mops_fast, mops_ref,
              mops_fast / mops_ref);
  std::printf("events %llu, fills %llu, cancels %llu, rejects %llu, final live orders %zu\n",
              static_cast<unsigned long long>(sf.events), static_cast<unsigned long long>(sf.fills),
              static_cast<unsigned long long>(sf.cancels), static_cast<unsigned long long>(sf.rejects), live_fast);
  bool same = sf.events == sr.events && sf.fills == sr.fills && sf.notional == sr.notional &&
              sf.filled_qty == sr.filled_qty && sf.cancels == sr.cancels && sf.rejects == sr.rejects && live_fast == live_ref;
  std::printf("optimized and reference agree on event counts and fill notional: %s\n", same ? "yes" : "NO (BUG)");
  return same ? 0 : 1;
}
