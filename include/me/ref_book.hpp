#pragma once
// Deliberately simple reference book: std::map price levels of std::list FIFOs plus std::unordered_map
// for id lookup. Implements the same spec as OrderBook (see order_book.hpp). It is both the oracle for
// differential testing and the "textbook design" baseline the optimized book is benchmarked against.
#include <algorithm>
#include <functional>
#include <list>
#include <map>
#include <unordered_map>

#include "order_book.hpp"  // BookConfig
#include "types.hpp"

namespace me {

template <class Sink>
class RefBook {
  struct RO {
    OrderId id;
    Qty qty;
  };
  using List = std::list<RO>;
  struct Loc {
    Side side;
    Price price;
    typename List::iterator it;
  };

 public:
  explicit RefBook(BookConfig c = {}, Sink sink = {}) : cfg_(c), sink_(std::move(sink)) {}

  void process(const Msg& m) {
    switch (m.type) {
      case MsgType::New: new_order(m, false); break;
      case MsgType::NewIOC: new_order(m, true); break;
      case MsgType::Cancel: cancel(m); break;
      case MsgType::Modify: modify(m); break;
    }
  }

  Sink& sink() { return sink_; }
  std::size_t live_orders() const { return idx_.size(); }
  Price best_bid() const { return bids_.empty() ? -1 : bids_.begin()->first; }
  Price best_ask() const { return asks_.empty() ? -1 : asks_.begin()->first; }

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
    if (idx_.count(m.id)) return reject(m, Reject::DuplicateId);
    if (idx_.size() >= cfg_.max_orders) return reject(m, Reject::PoolFull);
    emit(EvType::Accepted, m.side, m.id, 0, m.price, m.qty);
    submit(m.id, m.side, m.price, m.qty, ioc);
  }

  void submit(OrderId id, Side side, Price price, Qty qty, bool ioc) {
    qty = side == Side::Buy ? match(asks_, side, id, price, qty) : match(bids_, side, id, price, qty);
    if (qty == 0) return;
    if (ioc) return emit(EvType::Cancelled, side, id, 0, price, qty);
    List& lst = side == Side::Buy ? bids_[price] : asks_[price];
    lst.push_back({id, qty});
    idx_[id] = Loc{side, price, std::prev(lst.end())};
  }

  template <class Opp>
  Qty match(Opp& opp, Side taker_side, OrderId taker, Price limit, Qty qty) {
    while (qty && !opp.empty()) {
      auto lvl = opp.begin();
      const Price px = lvl->first;
      if (taker_side == Side::Buy ? px > limit : px < limit) break;
      List& lst = lvl->second;
      while (qty && !lst.empty()) {
        RO& mk = lst.front();
        const Qty f = std::min(qty, mk.qty);
        emit(EvType::Fill, taker_side, taker, mk.id, px, f);
        qty -= f;
        mk.qty -= f;
        if (!mk.qty) {
          idx_.erase(mk.id);
          lst.pop_front();
        }
      }
      if (lst.empty()) opp.erase(lvl);
    }
    return qty;
  }

  void remove(const Loc& loc) {
    if (loc.side == Side::Buy) {
      auto lv = bids_.find(loc.price);
      lv->second.erase(loc.it);
      if (lv->second.empty()) bids_.erase(lv);
    } else {
      auto lv = asks_.find(loc.price);
      lv->second.erase(loc.it);
      if (lv->second.empty()) asks_.erase(lv);
    }
  }

  void cancel(const Msg& m) {
    auto f = idx_.find(m.id);
    if (f == idx_.end()) return reject(m, Reject::UnknownId);
    const Loc loc = f->second;
    const Qty q = loc.it->qty;
    idx_.erase(f);
    remove(loc);
    emit(EvType::Cancelled, loc.side, m.id, 0, loc.price, q);
  }

  void modify(const Msg& m) {
    auto f = idx_.find(m.id);
    if (f == idx_.end()) return reject(m, Reject::UnknownId);
    if (m.qty == 0) return reject(m, Reject::BadQty);
    if (!price_ok(m.price)) return reject(m, Reject::BadPrice);
    Loc loc = f->second;
    if (m.price == loc.price && m.qty <= loc.it->qty) {
      loc.it->qty = m.qty;
      return emit(EvType::Modified, loc.side, m.id, 0, loc.price, m.qty);
    }
    idx_.erase(f);
    remove(loc);
    emit(EvType::Modified, loc.side, m.id, 0, m.price, m.qty);
    submit(m.id, loc.side, m.price, m.qty, false);
  }

  BookConfig cfg_;
  Sink sink_;
  std::map<Price, List, std::greater<Price>> bids_;
  std::map<Price, List> asks_;
  std::unordered_map<OrderId, Loc> idx_;
};

}  // namespace me
