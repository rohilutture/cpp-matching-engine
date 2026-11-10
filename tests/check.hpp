#pragma once
// Minimal test harness (no external dependencies).
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

struct TestCase { const char* name; std::function<void()> fn; };
inline std::vector<TestCase>& registry() { static std::vector<TestCase> r; return r; }
inline int& failures() { static int f = 0; return f; }
struct Registrar { Registrar(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };

#define TEST(name) \
  static void test_##name(); \
  static Registrar reg_##name(#name, test_##name); \
  static void test_##name()

#define CHECK(c) \
  do { if (!(c)) { std::fprintf(stderr, "  CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures(); } } while (0)
#define CHECK_EQ(a, b) \
  do { auto va_ = (a); auto vb_ = (b); if (!(va_ == vb_)) { \
    std::fprintf(stderr, "  CHECK_EQ failed %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, \
                 (long long)va_, (long long)vb_); ++failures(); } } while (0)
#define CHECK_MSG(c, msg) \
  do { if (!(c)) { std::fprintf(stderr, "  CHECK failed %s:%d: %s [%s]\n", __FILE__, __LINE__, #c, std::string(msg).c_str()); ++failures(); } } while (0)
