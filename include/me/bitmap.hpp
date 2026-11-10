#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace me {

// Two-level bitmap over price levels: l0 has one bit per level, l1 one bit per non-zero l0 word.
// next_set / prev_set find the closest non-empty level in O(words / 64) worst case, typically O(1).
class LevelBitmap {
 public:
  static constexpr std::size_t npos = static_cast<std::size_t>(-1);

  explicit LevelBitmap(std::size_t n) : n_(n), l0_((n + 63) / 64, 0), l1_((l0_.size() + 63) / 64, 0) {}

  void set(std::size_t i) {
    l0_[i >> 6] |= 1ull << (i & 63);
    l1_[i >> 12] |= 1ull << ((i >> 6) & 63);
  }
  void clear(std::size_t i) {
    std::uint64_t& w = l0_[i >> 6];
    w &= ~(1ull << (i & 63));
    if (!w) l1_[i >> 12] &= ~(1ull << ((i >> 6) & 63));
  }
  bool test(std::size_t i) const { return (l0_[i >> 6] >> (i & 63)) & 1; }

  // Smallest set index >= i, or npos.
  std::size_t next_set(std::size_t i) const {
    if (i >= n_) return npos;
    std::size_t w = i >> 6;
    if (std::uint64_t m = l0_[w] & (~0ull << (i & 63))) return (w << 6) + std::countr_zero(m);
    std::size_t w1 = w + 1;
    if (w1 >= l0_.size()) return npos;
    std::size_t s = w1 >> 6;
    std::uint64_t sm = l1_[s] & (~0ull << (w1 & 63));
    for (;;) {
      if (sm) {
        std::size_t word = (s << 6) + std::countr_zero(sm);
        return (word << 6) + std::countr_zero(l0_[word]);
      }
      if (++s >= l1_.size()) return npos;
      sm = l1_[s];
    }
  }

  // Largest set index <= i, or npos.
  std::size_t prev_set(std::size_t i) const {
    if (n_ == 0) return npos;
    if (i >= n_) i = n_ - 1;
    std::size_t w = i >> 6;
    if (std::uint64_t m = l0_[w] & (~0ull >> (63 - (i & 63)))) return (w << 6) + 63 - std::countl_zero(m);
    if (w == 0) return npos;
    std::size_t w1 = w - 1;
    std::size_t s = w1 >> 6;
    std::uint64_t sm = l1_[s] & (~0ull >> (63 - (w1 & 63)));
    for (;;) {
      if (sm) {
        std::size_t word = (s << 6) + 63 - std::countl_zero(sm);
        return (word << 6) + 63 - std::countl_zero(l0_[word]);
      }
      if (s == 0) return npos;
      sm = l1_[--s];
    }
  }

  std::size_t size() const { return n_; }

 private:
  std::size_t n_;
  std::vector<std::uint64_t> l0_, l1_;
};

}  // namespace me
