#pragma once
// Price-time priority limit order book, built for a zero-allocation hot path.
//
// Spec (shared with RefBook; the differential fuzz test enforces they agree event for event):
//   New / NewIOC  validate in order: id==0 BadId, qty==0 BadQty, price outside [min,max) BadPrice,
//                 id already resting DuplicateId, no free pool slot PoolFull. Then emit Accepted, match
//                 against the opposite side (best price first, FIFO within a level, trade at the maker's
//                 price, one Fill event per maker touched). Remainder: GTC rests; IOC emits Cancelled.
//   Cancel        unknown id -> Rejected(UnknownId); else Cancelled(remaining qty).
//   Modify        validate: UnknownId, then qty==0 BadQty, then price BadPrice. Same price and qty <= old:
//                 in-place reduction keeps time priority, emit Modified. Otherwise cancel/replace: the order
//                 loses priority, emit Modified, then match like a new GTC order (may fill, remainder rests).
//
// Layout: levels are a flat array indexed by (price - min), each level an intrusive doubly linked FIFO of
// orders from a preallocated pool (free list threaded through `next`); id -> slot via IdMap; best price
// via a two-level bitmap. Nothing on the hot path allocates.
#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "bitmap.hpp"
#include "id_map.hpp"
#include "types.hpp"

namespace me {

struct BookConfig {
  Price min_price = 0;
  Price max_price = 65536;  // exclusive
  std::uint32_t max_orders = 1u << 20;
};

template <class Sink>
class OrderBook {
  static constexpr std::size_t npos = LevelBitmap::npos;

  struct Order {
    OrderId id;
    Qty qty;
    std::uint32_t prev, next;
    Price price;
    Side side;
  };
  struct Level {
    std::uint32_t head = 0, tail = 0;
    Qty qty = 0;
    std::uint32_t pad = 0;
  };
  struct SideBook {
    std::vector<Level> lv;
    LevelBitmap bm;
    std::size_t best = npos;
    explicit SideBook(std::size_t n) : lv(n), bm(n) {}
  };

 public:
  explicit OrderBook(BookConfig c = {}, Sink sink = {})
      : cfg_(c),
        sink_(std::move(sink)),
        n_(static_cast<std::size_t>(c.max_price - c.min_price)),
        side_{SideBook(n_), SideBook(n_)},
        pool_(static_cast<std::size_t>(c.max_orders) + 1),
        ids_(c.max_orders) {
    for (std::uint32_t i = 1; i <= c.max_orders; ++i) pool_[i].next = (i == c.max_orders) ? 0 : i + 1;
    free_head_ = c.max_orders ? 1 : 0;
  }

  void process(const Msg& m) {
    switch (m.type) {
      case MsgType::New: new_order(m, false); break;
      case MsgType::NewIOC: new_order(m, true); break;
      case MsgType::Cancel: cancel(m); break;
      case MsgType::Modify: modify(m); break;
    }
  }

  Sink& sink() { return sink_; }
  std::size_t live_orders() const { return ids_.size(); }
  std::size_t best_bid_idx() const { return side_[0].best; }
  std::size_t best_ask_idx() const { return side_[1].best; }
  Price best_bid() const { return side_[0].best == npos ? -1 : cfg_.min_price + static_cast<Price>(side_[0].best); }
  Price best_ask() const { return side_[1].best == npos ? -1 : cfg_.min_price + static_cast<Price>(side_[1].best); }
  Qty level_qty(Side s, Price p) const { return side_[static_cast<int>(s)].lv[static_cast<std::size_t>(p - cfg_.min_price)].qty; }

  // O(n) structural check, for tests only. Returns an empty string if consistent, else a description.
  std::string validate() const {
    std::size_t live = 0;
    for (int sd = 0; sd < 2; ++sd) {
      const SideBook& sb = side_[sd];
      std::size_t best = npos;
      for (std::size_t i = 0; i < n_; ++i) {
        const Level& L = sb.lv[i];
        std::size_t cnt = 0;
        Qty sum = 0;
        std::uint32_t prev = 0;
        for (std::uint32_t s = L.head; s; s = pool_[s].next) {
          const Order& o = pool_[s];
          if (o.prev != prev) return "bad prev link";
          if (o.price != cfg_.min_price + static_cast<Price>(i) || static_cast<int>(o.side) != sd) return "order in wrong level";
          if (o.qty == 0) return "zero-qty resting order";
          if (ids_.find(o.id) != s) return "id map mismatch";
          sum += o.qty;
          prev = s;
          ++cnt;
        }
        if (L.tail != prev) return "bad tail";
        if (sum != L.qty) return "level qty mismatch";
        if (sb.bm.test(i) != (cnt > 0)) return "bitmap mismatch";
        if (cnt) {
          live += cnt;
          if (sd == 0) best = i;  // highest bid
          else if (best == npos) best = i;  // lowest ask
        }
      }
      if (best != sb.best) return "best mismatch";
    }
    if (live != ids_.size()) return "live count mismatch";
    if (side_[0].best != npos && side_[1].best != npos && side_[0].best >= side_[1].best) return "book crossed";
    return {};
  }

 private:
  void emit(EvType t, Side s, OrderId id, OrderId other, Price p, Qty q, Reject r = Reject::None) {
    sink_(make_event(t, s, id, other, p, q, r));
  }
  void reject(const Msg& m, Reject r) { emit(EvType::Rejected, m.side, m.id, 0, m.price, m.qty, r); }
  bool price_ok(Price p) const { return p >= cfg_.min_price && p < cfg_.max_price; }

  void new_order(const Msg& m, bool ioc) {
    if (m.id == 0) return reject(m, Reject::BadId);
    if (m.qty == 0) return reject(m, Reject::BadQty);
    if (!price_ok(m.price)) return reject(m, Reject::BadPrice);
    if (ids_.find(m.id)) return reject(m, Reject::DuplicateId);
    if (free_head_ == 0) return reject(m, Reject::PoolFull);
    emit(EvType::Accepted, m.side, m.id, 0, m.price, m.qty);
    submit(m.id, m.side, m.price, m.qty, ioc);
  }

  void submit(OrderId id, Side side, Price price, Qty qty, bool ioc) {
    const std::size_t pidx = static_cast<std::size_t>(price - cfg_.min_price);
    qty = (side == Side::Buy) ? match<true>(id, pidx, qty) : match<false>(id, pidx, qty);
    if (qty == 0) return;
    if (ioc) return emit(EvType::Cancelled, side, id, 0, price, qty);
    rest(id, side, pidx, price, qty);
  }

  template <bool BUY>
  Qty match(OrderId taker, std::size_t pidx, Qty qty) {
    SideBook& opp = side_[BUY ? 1 : 0];
    constexpr Side taker_side = BUY ? Side::Buy : Side::Sell;
    while (qty && opp.best != npos && (BUY ? opp.best <= pidx : opp.best >= pidx)) {
      const std::size_t bi = opp.best;
      Level& L = opp.lv[bi];
      const Price px = cfg_.min_price + static_cast<Price>(bi);
      while (qty && L.head) {
        const std::uint32_t s = L.head;
        Order& mk = pool_[s];
        const Qty f = std::min(qty, mk.qty);
        emit(EvType::Fill, taker_side, taker, mk.id, px, f);
        qty -= f;
        mk.qty -= f;
        L.qty -= f;
        if (!mk.qty) {
          L.head = mk.next;
          if (L.head) pool_[L.head].prev = 0; else L.tail = 0;
          ids_.erase(mk.id);
          mk.next = free_head_;
          free_head_ = s;
        }
      }
      if (!L.head) {
        opp.bm.clear(bi);
        opp.best = BUY ? opp.bm.next_set(bi) : opp.bm.prev_set(bi);
      }
    }
    return qty;
  }

  void rest(OrderId id, Side side, std::size_t pidx, Price price, Qty qty) {
    const std::uint32_t s = free_head_;
    free_head_ = pool_[s].next;
    SideBook& sb = side_[static_cast<int>(side)];
    Level& L = sb.lv[pidx];
    pool_[s] = Order{id, qty, L.tail, 0, price, side};
    if (L.tail) pool_[L.tail].next = s; else L.head = s;
    L.tail = s;
    L.qty += qty;
    sb.bm.set(pidx);
    if (side == Side::Buy) { if (sb.best == npos || pidx > sb.best) sb.best = pidx; }
    else if (sb.best == npos || pidx < sb.best) sb.best = pidx;
    ids_.insert(id, s);
  }

  void unlink(std::uint32_t s) {
    Order& o = pool_[s];
    SideBook& sb = side_[static_cast<int>(o.side)];
    const std::size_t pidx = static_cast<std::size_t>(o.price - cfg_.min_price);
    Level& L = sb.lv[pidx];
    if (o.prev) pool_[o.prev].next = o.next; else L.head = o.next;
    if (o.next) pool_[o.next].prev = o.prev; else L.tail = o.prev;
    L.qty -= o.qty;
    if (!L.head) {
      sb.bm.clear(pidx);
      if (sb.best == pidx) sb.best = (o.side == Side::Buy) ? sb.bm.prev_set(pidx) : sb.bm.next_set(pidx);
    }
    ids_.erase(o.id);
    o.next = free_head_;
    free_head_ = s;
  }

  void cancel(const Msg& m) {
    const std::uint32_t s = ids_.find(m.id);
    if (!s) return reject(m, Reject::UnknownId);
    const Order o = pool_[s];
    unlink(s);
    emit(EvType::Cancelled, o.side, o.id, 0, o.price, o.qty);
  }

  void modify(const Msg& m) {
    const std::uint32_t s = ids_.find(m.id);
    if (!s) return reject(m, Reject::UnknownId);
    if (m.qty == 0) return reject(m, Reject::BadQty);
    if (!price_ok(m.price)) return reject(m, Reject::BadPrice);
    Order& o = pool_[s];
    if (m.price == o.price && m.qty <= o.qty) {  // in-place reduction keeps priority
      side_[static_cast<int>(o.side)].lv[static_cast<std::size_t>(o.price - cfg_.min_price)].qty -= o.qty - m.qty;
      o.qty = m.qty;
      return emit(EvType::Modified, o.side, o.id, 0, o.price, o.qty);
    }
    const Side sd = o.side;
    unlink(s);
    emit(EvType::Modified, sd, m.id, 0, m.price, m.qty);
    submit(m.id, sd, m.price, m.qty, false);
  }

  BookConfig cfg_;
  Sink sink_;
  std::size_t n_;
  std::array<SideBook, 2> side_;  // [0] bids, [1] asks
  std::vector<Order> pool_;       // slot 0 is the null sentinel
  IdMap ids_;
  std::uint32_t free_head_ = 0;
};

}  // namespace me
