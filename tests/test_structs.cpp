#include <random>
#include <set>
#include <thread>
#include <unordered_map>

#include "check.hpp"
#include "me/bitmap.hpp"
#include "me/id_map.hpp"
#include "me/spsc_queue.hpp"

using namespace me;

TEST(bitmap_matches_std_set) {
  for (std::size_t n : {1u, 63u, 64u, 65u, 4095u, 4096u, 4097u, 10000u, 65536u}) {
    LevelBitmap bm(n);
    std::set<std::size_t> ref;
    std::mt19937_64 rng(n);
    for (int it = 0; it < 20000; ++it) {
      std::size_t i = rng() % n;
      if (rng() & 1) { bm.set(i); ref.insert(i); }
      else { bm.clear(i); ref.erase(i); }
      std::size_t q = rng() % (n + 3);  // includes out-of-range queries
      auto nx = ref.lower_bound(q);
      CHECK_EQ(bm.next_set(q), nx == ref.end() ? LevelBitmap::npos : *nx);
      std::size_t pv = LevelBitmap::npos;
      auto ub = ref.upper_bound(std::min(q, n - 1));
      if (ub != ref.begin()) pv = *std::prev(ub);
      CHECK_EQ(bm.prev_set(q), pv);
    }
  }
}

TEST(id_map_matches_unordered_map_under_collisions) {
  // keys that are multiples of a large power of two cluster under a weak hash; stress the backward shift
  for (std::uint64_t stride : {1ull, 1024ull, 1ull << 32}) {
    IdMap m(2000);
    std::unordered_map<OrderId, std::uint32_t> ref;
    std::mt19937_64 rng(stride);
    for (int it = 0; it < 200000; ++it) {
      OrderId k = (rng() % 3000 + 1) * stride;
      if (ref.count(k)) {
        if (rng() % 3 == 0) { m.erase(k); ref.erase(k); }
      } else if (ref.size() < 2000) {
        std::uint32_t v = 1 + rng() % 1000000;
        m.insert(k, v); ref[k] = v;
      }
      OrderId probe = (rng() % 3000 + 1) * stride;
      auto f = ref.find(probe);
      CHECK_EQ(m.find(probe), f == ref.end() ? 0u : f->second);
    }
    CHECK_EQ(m.size(), ref.size());
    for (auto& [k, v] : ref) CHECK_EQ(m.find(k), v);
  }
}

TEST(id_map_find_zero_always_misses_even_after_erases) {
  // regression: key 0 is the empty marker; an erased slot used to keep a stale value that find(0) returned
  IdMap m(16);
  for (OrderId k = 1; k <= 10; ++k) m.insert(k, static_cast<std::uint32_t>(k + 100));
  for (OrderId k = 1; k <= 10; ++k) m.erase(k);
  CHECK_EQ(m.find(0), 0u);
  m.insert(7, 55);
  CHECK_EQ(m.find(0), 0u);
  CHECK_EQ(m.find(7), 55u);
}

TEST(spsc_two_threads_preserve_order_and_lose_nothing) {
  constexpr std::uint64_t N = 5'000'000;
  static SpscQueue<std::uint64_t, 1 << 12> q;
  std::thread prod([&] {
    for (std::uint64_t i = 1; i <= N; ++i) while (!q.try_push(i)) {}
  });
  std::uint64_t expect = 1, v = 0;
  while (expect <= N) {
    if (q.try_pop(v)) { if (v != expect) { CHECK_EQ(v, expect); break; } ++expect; }
  }
  prod.join();
  CHECK(!q.try_pop(v));
}

TEST(spsc_batch_pop_preserves_order_and_loses_nothing) {
  constexpr std::uint64_t N = 3'000'000;
  static SpscQueue<std::uint64_t, 1 << 10> q;
  std::thread prod([&] {
    for (std::uint64_t i = 1; i <= N; ++i) while (!q.try_push(i)) {}
  });
  std::uint64_t expect = 1, buf[37];  // odd batch size so bursts straddle the ring boundary
  while (expect <= N) {
    std::size_t k = q.try_pop_batch(buf, 37);
    for (std::size_t i = 0; i < k; ++i, ++expect) {
      if (buf[i] != expect) { CHECK_EQ(buf[i], expect); expect = N + 1; break; }
    }
  }
  prod.join();
  CHECK_EQ(q.try_pop_batch(buf, 37), 0u);
}

TEST(spsc_batch_pop_edges) {
  SpscQueue<int, 8> q;
  int out[8];
  CHECK_EQ(q.try_pop_batch(out, 8), 0u);
  for (int i = 0; i < 5; ++i) q.try_push(i);
  CHECK_EQ(q.try_pop_batch(out, 3), 3u);  // respects max
  CHECK_EQ(out[2], 2);
  CHECK_EQ(q.try_pop_batch(out, 8), 2u);  // returns only what is available
  CHECK(out[0] == 3 && out[1] == 4);
  for (int r = 0; r < 20; ++r) {          // wraparound with batches
    for (int i = 0; i < 6; ++i) CHECK(q.try_push(r * 10 + i));
    CHECK_EQ(q.try_pop_batch(out, 8), 6u);
    for (int i = 0; i < 6; ++i) CHECK_EQ(out[i], r * 10 + i);
  }
}

TEST(spsc_full_and_empty_edges) {
  SpscQueue<int, 4> q;
  int v = 0;
  CHECK(!q.try_pop(v));
  for (int i = 0; i < 4; ++i) CHECK(q.try_push(i));
  CHECK(!q.try_push(99));  // full
  for (int i = 0; i < 4; ++i) { CHECK(q.try_pop(v)); CHECK_EQ(v, i); }
  CHECK(!q.try_pop(v));
  for (int i = 0; i < 10; ++i) { CHECK(q.try_push(i)); CHECK(q.try_pop(v)); CHECK_EQ(v, i); }  // wraparound
}
