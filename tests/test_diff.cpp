// Differential fuzz: the optimized OrderBook and the naive RefBook must emit identical event streams on
// the same random message sequence, including garbage input. validate() checks the optimized book's
// structure (links, level sums, bitmap, best prices, id map, not crossed) throughout.
#include <random>
#include <vector>

#include "check.hpp"
#include "me/order_book.hpp"
#include "me/ref_book.hpp"

using namespace me;

namespace {
struct VecSink {
  std::vector<Event> ev;
  void operator()(const Event& e) { ev.push_back(e); }
};

struct FuzzCfg {
  const char* name;
  BookConfig book;
  int price_span;      // prices drawn from [mid - span, mid + span] (may exceed the valid range on purpose)
  int id_space;        // ids drawn from [0, id_space] -> plenty of duplicates / unknown / zero
  int n_msgs;
  std::uint64_t seed;
};

Msg random_msg(std::mt19937_64& rng, const FuzzCfg& c, Price mid) {
  Msg m{};
  m.id = rng() % (c.id_space + 1);  // 0 is invalid on purpose
  m.side = (rng() & 1) ? Side::Buy : Side::Sell;
  m.price = mid + static_cast<Price>(static_cast<int>(rng() % (2 * c.price_span + 1)) - c.price_span);
  m.qty = (rng() % 20 == 0) ? 0 : 1 + rng() % 30;  // occasional zero qty
  unsigned r = rng() % 100;
  m.type = r < 55 ? MsgType::New : r < 65 ? MsgType::NewIOC : r < 85 ? MsgType::Cancel : MsgType::Modify;
  return m;
}

struct Coverage {
  std::size_t fills = 0, cancels = 0, modifies = 0, accepted = 0;
  std::size_t reasons[7] = {};
};

Coverage run(const FuzzCfg& c) {
  Coverage cov;
  OrderBook<VecSink> fast(c.book);
  RefBook<VecSink> ref(c.book);
  std::mt19937_64 rng(c.seed);
  Price mid = (c.book.min_price + c.book.max_price) / 2;
  for (int i = 0; i < c.n_msgs; ++i) {
    if (i % 500 == 0) mid += static_cast<Price>(rng() % 5) - 2;  // drifting mid keeps levels turning over
    Msg m = random_msg(rng, c, mid);
    fast.process(m);
    ref.process(m);
    auto& a = fast.sink().ev;
    auto& b = ref.sink().ev;
    if (a != b) {
      std::size_t k = 0;
      while (k < a.size() && k < b.size() && a[k] == b[k]) ++k;
      auto d = [](const std::vector<Event>& v, std::size_t k) {
        if (k >= v.size()) return std::string("<none>");
        const Event& e = v[k];
        return "type=" + std::to_string(int(e.type)) + " reason=" + std::to_string(int(e.reason)) +
               " id=" + std::to_string(e.id) + " other=" + std::to_string(e.other) + " px=" + std::to_string(e.price) +
               " qty=" + std::to_string(e.qty);
      };
      CHECK_MSG(false, std::string(c.name) + ": event streams diverge at msg " + std::to_string(i) + " (event " +
                           std::to_string(k) + ", sizes " + std::to_string(a.size()) + " vs " + std::to_string(b.size()) +
                           ")\n    msg: type=" + std::to_string(int(m.type)) + " id=" + std::to_string(m.id) +
                           " px=" + std::to_string(m.price) + " qty=" + std::to_string(m.qty) +
                           " live fast=" + std::to_string(fast.live_orders()) + " ref=" + std::to_string(ref.live_orders()) +
                           "\n    fast: " + d(a, k) + "\n    ref:  " + d(b, k));
      return cov;
    }
    for (const Event& e : a) {
      cov.fills += e.type == EvType::Fill;
      cov.cancels += e.type == EvType::Cancelled;
      cov.modifies += e.type == EvType::Modified;
      cov.accepted += e.type == EvType::Accepted;
      cov.reasons[static_cast<int>(e.reason)] += e.type == EvType::Rejected;
    }
    a.clear();
    b.clear();
    if (i % 4000 == 0) {
      auto v = fast.validate();
      CHECK_MSG(v.empty(), std::string(c.name) + " msg " + std::to_string(i) + ": " + v);
      if (!v.empty()) return cov;
    }
    if (fast.live_orders() != ref.live_orders() || fast.best_bid() != ref.best_bid() || fast.best_ask() != ref.best_ask()) {
      CHECK_MSG(false, std::string(c.name) + ": state diverges at msg " + std::to_string(i));
      return cov;
    }
  }
  auto v = fast.validate();
  CHECK_MSG(v.empty(), std::string(c.name) + " final: " + v);
  return cov;
}

// A passing fuzz is only meaningful if it actually exercised the paths: require real volume of each.
void require_coverage(const char* name, const Coverage& cv, bool pool_full) {
  auto R = [&](Reject r) { return cv.reasons[static_cast<int>(r)]; };
  std::string s = std::string(name) + " fills=" + std::to_string(cv.fills) + " cancels=" + std::to_string(cv.cancels) +
                  " modifies=" + std::to_string(cv.modifies);
  CHECK_MSG(cv.fills > 1000 && cv.cancels > 1000 && cv.modifies > 500 && cv.accepted > 1000, s);
  CHECK_MSG(R(Reject::BadId) > 50 && R(Reject::BadQty) > 50 && R(Reject::UnknownId) > 50 && R(Reject::DuplicateId) > 50, s);
  if (pool_full) CHECK_MSG(R(Reject::PoolFull) > 50, s + " (no PoolFull rejects)");
}
}  // namespace

TEST(diff_wide_book) {
  auto cv = run({"wide", {0, 4096, 100000}, 200, 3000, 400000, 1});
  require_coverage("wide", cv, false);  // prices never leave the valid range here; narrow covers BadPrice
}
TEST(diff_narrow_book_heavy_crossing) {
  auto cv = run({"narrow", {0, 64, 100000}, 40, 2000, 400000, 2});  // price span exceeds range: BadPrice too
  require_coverage("narrow", cv, false);
  CHECK(cv.reasons[static_cast<int>(Reject::BadPrice)] > 50);
}
TEST(diff_tiny_pool_exercises_pool_full) {
  require_coverage("pool", run({"pool", {0, 512, 40}, 30, 500, 300000, 3}), true);
}
TEST(diff_few_ids_many_duplicates) {
  require_coverage("ids", run({"ids", {0, 256, 1000}, 20, 30, 300000, 4}), false);
}
TEST(diff_many_seeds_small) {
  for (std::uint64_t s = 10; s < 40; ++s) run({"seeds", {0, 128, 200}, 25, 300, 40000, s});
}
