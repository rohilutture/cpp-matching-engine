#pragma once
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace me {

// Bounded single-producer single-consumer lock-free ring buffer.
//  * head (consumer) and tail (producer) live on separate cache lines, with no false sharing.
//  * Each side keeps a cached copy of the other side's index, so the shared atomic is only re-read
//    (acquire) when the queue looks full/empty. In steady state this removes cross-core traffic.
//  * Only release stores on publish and acquire loads on refresh; no RMW instructions anywhere.
// Exactly one producer thread and one consumer thread may use it. T must be trivially copyable.
template <class T, std::size_t N>
class SpscQueue {
  static_assert(std::has_single_bit(N), "capacity must be a power of two");

 public:
  // aligned_alloc requires size to be a multiple of the alignment (UB otherwise; ASan enforces it)
  SpscQueue() : buf_(static_cast<T*>(std::aligned_alloc(64, (N * sizeof(T) + 63) & ~std::size_t{63}))) {
    if (!buf_) throw std::bad_alloc();
  }
  ~SpscQueue() { std::free(buf_); }
  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;

  bool try_push(const T& v) {
    const std::size_t t = p_.tail.load(std::memory_order_relaxed);
    if (t - p_.cached_head == N) {
      p_.cached_head = c_.head.load(std::memory_order_acquire);
      if (t - p_.cached_head == N) return false;
    }
    buf_[t & (N - 1)] = v;
    p_.tail.store(t + 1, std::memory_order_release);
    return true;
  }

  bool try_pop(T& v) {
    const std::size_t h = c_.head.load(std::memory_order_relaxed);
    if (h == c_.cached_tail) {
      c_.cached_tail = p_.tail.load(std::memory_order_acquire);
      if (h == c_.cached_tail) return false;
    }
    v = buf_[h & (N - 1)];
    c_.head.store(h + 1, std::memory_order_release);
    return true;
  }

  // Pop up to `max` already-available items in one go (never waits). Copying a burst issues independent
  // loads, so the cross-core cache-line transfers overlap instead of each being exposed one at a time
  // between slow consumer iterations; it also pays the acquire/release pair once per burst.
  std::size_t try_pop_batch(T* out, std::size_t max) {
    const std::size_t h = c_.head.load(std::memory_order_relaxed);
    std::size_t avail = c_.cached_tail - h;
    if (avail == 0) {
      c_.cached_tail = p_.tail.load(std::memory_order_acquire);
      avail = c_.cached_tail - h;
      if (avail == 0) return 0;
    }
    const std::size_t n = avail < max ? avail : max;
    for (std::size_t i = 0; i < n; ++i) out[i] = buf_[(h + i) & (N - 1)];
    c_.head.store(h + n, std::memory_order_release);
    return n;
  }

  static constexpr std::size_t capacity() { return N; }

 private:
  struct alignas(64) Cons {
    std::atomic<std::size_t> head{0};
    std::size_t cached_tail = 0;
  };
  struct alignas(64) Prod {
    std::atomic<std::size_t> tail{0};
    std::size_t cached_head = 0;
  };
  Cons c_;
  Prod p_;
  T* buf_;
};

}  // namespace me
