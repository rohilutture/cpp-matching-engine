#pragma once
// Synthetic order flow. NOT real market data: a random-walk mid price, passive orders placed at a
// geometric distance from mid, a fraction of marketable (crossing) orders, cancels of orders the generator
// believes are live (some were already filled, so they become UnknownId rejects, as in real feeds),
// and modifies. Deterministic for a given seed.
#include <vector>

#include "types.hpp"

namespace me {

struct FlowConfig {
  std::uint64_t seed = 1;
  std::size_t n = 4'000'000;
  Price mid = 32768;
  double p_passive = 0.45, p_aggressive = 0.12, p_cancel = 0.33;  // remainder: modify
};

class Rng {  // xorshift64*
 public:
  explicit Rng(std::uint64_t s) : s_(s ? s : 0x9E3779B97F4A7C15ull) {}
  std::uint64_t next() {
    s_ ^= s_ >> 12; s_ ^= s_ << 25; s_ ^= s_ >> 27;
    return s_ * 0x2545F4914F6CDD1Dull;
  }
  double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
  std::uint64_t below(std::uint64_t n) { return next() % n; }
 private:
  std::uint64_t s_;
};

inline std::vector<Msg> generate_flow(const FlowConfig& c) {
  Rng rng(c.seed);
  std::vector<Msg> out;
  out.reserve(c.n);
  std::vector<OrderId> live;
  OrderId next_id = 1;
  Price mid = c.mid;
  for (std::size_t i = 0; i < c.n; ++i) {
    if (i % 64 == 0) mid = std::clamp<Price>(mid + static_cast<Price>(rng.below(3)) - 1, 2000, 60000);
    double r = rng.uniform();
    Msg m{};
    if (r < c.p_passive) {
      m.type = MsgType::New;
      m.side = rng.below(2) ? Side::Buy : Side::Sell;
      int off = 1;
      while (off < 60 && rng.uniform() < 0.65) ++off;
      m.price = m.side == Side::Buy ? mid - off : mid + off;
      m.qty = 1 + static_cast<Qty>(rng.below(100));
      m.id = next_id++;
      live.push_back(m.id);
    } else if (r < c.p_passive + c.p_aggressive) {
      m.type = rng.uniform() < 0.85 ? MsgType::New : MsgType::NewIOC;
      m.side = rng.below(2) ? Side::Buy : Side::Sell;
      Price k = static_cast<Price>(rng.below(6));
      m.price = m.side == Side::Buy ? mid + k : mid - k;
      m.qty = 1 + static_cast<Qty>(rng.below(150));
      m.id = next_id++;
      if (m.type == MsgType::New) live.push_back(m.id);
    } else if (r < c.p_passive + c.p_aggressive + c.p_cancel && !live.empty()) {
      std::size_t j = rng.below(live.size());
      m.type = MsgType::Cancel;
      m.id = live[j];
      live[j] = live.back();
      live.pop_back();
    } else if (!live.empty()) {
      m.type = MsgType::Modify;
      m.id = live[rng.below(live.size())];
      m.price = mid + static_cast<Price>(rng.below(21)) - 10;
      m.qty = 1 + static_cast<Qty>(rng.below(100));
    } else {
      m.type = MsgType::New; m.side = Side::Buy; m.price = mid - 1; m.qty = 1; m.id = next_id++;
      live.push_back(m.id);
    }
    out.push_back(m);
  }
  return out;
}

}  // namespace me
