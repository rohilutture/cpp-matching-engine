#include <chrono>
#include <cstring>

#include "check.hpp"

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int ran = 0;
  for (auto& t : registry()) {
    if (filter && !std::strstr(t.name, filter)) continue;
    int before = failures();
    auto t0 = std::chrono::steady_clock::now();
    t.fn();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("[%s] %-40s %8.1f ms\n", failures() == before ? " ok " : "FAIL", t.name, ms);
    ++ran;
  }
  std::printf("%d tests, %d failed checks\n", ran, failures());
  return failures() ? 1 : 0;
}
