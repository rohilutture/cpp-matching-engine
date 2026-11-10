#pragma once
#include <pthread.h>
#include <sched.h>
#include <x86intrin.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

namespace me {

inline std::uint64_t rdtsc() { return __rdtsc(); }
// Serialized timestamps for timing a region: lfence keeps earlier/later instructions out of the window.
inline std::uint64_t tsc_begin() { _mm_lfence(); std::uint64_t t = __rdtsc(); _mm_lfence(); return t; }
inline std::uint64_t tsc_end() { unsigned aux; std::uint64_t t = __rdtscp(&aux); _mm_lfence(); return t; }

// TSC frequency in GHz (cycles per ns), calibrated once against steady_clock over ~100 ms.
inline double tsc_ghz() {
  static const double g = [] {
    using clk = std::chrono::steady_clock;
    auto t0 = clk::now();
    std::uint64_t c0 = rdtsc();
    while (clk::now() - t0 < std::chrono::milliseconds(100)) {}
    auto t1 = clk::now();
    std::uint64_t c1 = rdtsc();
    return static_cast<double>(c1 - c0) / std::chrono::duration<double, std::nano>(t1 - t0).count();
  }();
  return g;
}

inline bool pin_to_cpu(int cpu) {
  cpu_set_t s;
  CPU_ZERO(&s);
  CPU_SET(cpu, &s);
  return pthread_setaffinity_np(pthread_self(), sizeof(s), &s) == 0;
}

struct Pcts { double p50, p90, p99, p999, max, mean; };

// Percentiles of cycle samples, converted to nanoseconds. Sorts `cyc` in place.
inline Pcts percentiles(std::vector<std::uint32_t>& cyc, std::uint64_t floor_cycles = 0) {
  Pcts p{};
  if (cyc.empty()) return p;
  std::sort(cyc.begin(), cyc.end());
  const double ns = 1.0 / tsc_ghz();
  auto at = [&](double q) {
    std::size_t i = std::min(cyc.size() - 1, static_cast<std::size_t>(q * static_cast<double>(cyc.size())));
    double c = static_cast<double>(cyc[i]);
    return std::max(0.0, c - static_cast<double>(floor_cycles)) * ns;
  };
  double sum = 0;
  for (auto c : cyc) sum += static_cast<double>(c);
  p = {at(0.50), at(0.90), at(0.99), at(0.999), at(1.0), (sum / static_cast<double>(cyc.size()) - static_cast<double>(floor_cycles)) * ns};
  return p;
}

// Median cost of an empty timed region (the measurement floor), in cycles.
inline std::uint64_t timer_floor_cycles() {
  std::vector<std::uint32_t> s(200000);
  for (auto& x : s) { std::uint64_t a = tsc_begin(); std::uint64_t b = tsc_end(); x = static_cast<std::uint32_t>(b - a); }
  std::sort(s.begin(), s.end());
  return s[s.size() / 2];
}

}  // namespace me
