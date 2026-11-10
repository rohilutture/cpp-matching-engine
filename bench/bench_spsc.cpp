// SPSC queue alone (no matching), 32-byte messages, across producer:consumer core pairs.
//   ./bench_spsc [--n 20000000] [--pairs 2:4,2:3,8:9]
// Isolates the cross-core transfer cost from the engine. Each pair is run 3 times; best and median reported.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>

#include "me/spsc_queue.hpp"
#include "me/timing.hpp"
#include "me/types.hpp"

using namespace me;

static double run_once(std::size_t n, int pc, int cc) {
  static SpscQueue<Msg, 1 << 16> q;
  std::atomic<bool> go{false};
  std::thread prod([&] {
    pin_to_cpu(pc);
    while (!go.load(std::memory_order_acquire)) {}
    Msg m{};
    for (std::size_t i = 0; i < n; ++i) { m.id = i + 1; while (!q.try_push(m)) {} }
  });
  pin_to_cpu(cc);
  auto t0 = std::chrono::steady_clock::now();
  go.store(true, std::memory_order_release);
  Msg m;
  std::uint64_t sum = 0;
  for (std::size_t i = 0; i < n;) if (q.try_pop(m)) { sum += m.id; ++i; }
  double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  prod.join();
  if (sum != n * (n + 1) / 2) { std::fprintf(stderr, "CHECKSUM MISMATCH\n"); std::exit(1); }
  return static_cast<double>(n) / s / 1e6;
}

int main(int argc, char** argv) {
  std::size_t n = 20'000'000;
  std::string pairs = "2:4,2:3,8:9,2:10";
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--n") && i + 1 < argc) n = std::strtoull(argv[++i], nullptr, 10);
    else if (!std::strcmp(argv[i], "--pairs") && i + 1 < argc) pairs = argv[++i];
  }
  std::printf("| prod:cons | best Mmsg/s | median Mmsg/s |\n|-----------|-------------|---------------|\n");
  std::stringstream ss(pairs);
  for (std::string tok; std::getline(ss, tok, ',');) {
    int pc = std::atoi(tok.c_str()), cc = std::atoi(tok.c_str() + tok.find(':') + 1);
    double r[3];
    for (double& x : r) x = run_once(n, pc, cc);
    std::sort(r, r + 3);
    std::printf("| %4d:%-4d | %11.1f | %13.1f |\n", pc, cc, r[2], r[1]);
  }
  return 0;
}
