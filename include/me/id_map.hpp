#pragma once
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "types.hpp"

namespace me {

// Open-addressing hash map OrderId -> pool slot (uint32, 0 = absent). Linear probing, power-of-two
// capacity (load <= 0.5), multiplicative hash, and backward-shift deletion (Knuth 6.4 Algorithm R)
// so there are no tombstones and lookups never degrade. Key 0 is the empty marker.
class IdMap {
 public:
  explicit IdMap(std::size_t max_entries)
      : cap_(std::bit_ceil(std::max<std::size_t>(max_entries * 2, 16))),
        mask_(cap_ - 1),
        shift_(64 - std::countr_zero(cap_)),
        slots_(cap_) {}

  std::uint32_t find(OrderId key) const {
    for (std::size_t i = home(key);; i = (i + 1) & mask_) {
      const Slot& s = slots_[i];
      if (s.key == 0) return 0;  // empty first: key 0 is the empty marker, so find(0) must always miss
      if (s.key == key) return s.val;
    }
  }

  void insert(OrderId key, std::uint32_t val) {  // caller guarantees key absent and not 0
    std::size_t i = home(key);
    while (slots_[i].key != 0) i = (i + 1) & mask_;
    slots_[i] = {key, val, 0};
    ++size_;
  }

  void erase(OrderId key) {  // caller guarantees key present
    std::size_t i = home(key);
    while (slots_[i].key != key) i = (i + 1) & mask_;
    for (;;) {
      std::size_t j = i;
      for (;;) {
        j = (j + 1) & mask_;
        if (slots_[j].key == 0) {
          slots_[i] = {0, 0, 0};
          --size_;
          return;
        }
        std::size_t k = home(slots_[j].key);
        bool stays = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);  // home cyclically in (i, j]
        if (!stays) break;
      }
      slots_[i] = slots_[j];
      i = j;
    }
  }

  std::size_t size() const { return size_; }

 private:
  struct Slot {
    OrderId key;
    std::uint32_t val;
    std::uint32_t pad;
  };
  std::size_t home(OrderId k) const { return static_cast<std::size_t>((k * 0x9E3779B97F4A7C15ull) >> shift_); }

  std::size_t cap_, mask_;
  int shift_;
  std::vector<Slot> slots_;
  std::size_t size_ = 0;
};

}  // namespace me
