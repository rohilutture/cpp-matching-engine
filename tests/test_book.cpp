#include <vector>

#include "check.hpp"
#include "me/order_book.hpp"

using namespace me;

namespace {
struct VecSink {
  std::vector<Event> ev;
  void operator()(const Event& e) { ev.push_back(e); }
};
using Book = OrderBook<VecSink>;

Msg msg(MsgType t, OrderId id, Side s, Price p, Qty q) { return Msg{0, id, p, q, s, t}; }
Msg buy(OrderId id, Price p, Qty q) { return msg(MsgType::New, id, Side::Buy, p, q); }
Msg sell(OrderId id, Price p, Qty q) { return msg(MsgType::New, id, Side::Sell, p, q); }
Msg cancel(OrderId id) { return msg(MsgType::Cancel, id, Side::Buy, 0, 0); }
Msg modify(OrderId id, Price p, Qty q) { return msg(MsgType::Modify, id, Side::Buy, p, q); }

BookConfig cfg(Price lo = 0, Price hi = 1000, std::uint32_t n = 1000) { return BookConfig{lo, hi, n}; }

std::vector<Event> fills(Book& b) {
  std::vector<Event> f;
  for (auto& e : b.sink().ev) if (e.type == EvType::Fill) f.push_back(e);
  return f;
}
}  // namespace

// Book exposes sink() non-const only; give the test helper a const view.
#define SINK(b) (b).sink().ev

TEST(book_rests_and_tracks_best) {
  Book b(cfg());
  b.process(buy(1, 100, 10));
  b.process(buy(2, 105, 5));
  b.process(sell(3, 120, 7));
  b.process(sell(4, 110, 3));
  CHECK_EQ(b.best_bid(), 105);
  CHECK_EQ(b.best_ask(), 110);
  CHECK_EQ(b.live_orders(), 4u);
  CHECK_MSG(b.validate().empty(), b.validate());
}

TEST(book_price_time_priority_and_trade_at_maker_price) {
  Book b(cfg());
  b.process(sell(1, 100, 5));   // first at 100
  b.process(sell(2, 100, 5));   // second at 100
  b.process(sell(3, 99, 5));    // better price, arrived last
  SINK(b).clear();
  b.process(buy(10, 105, 12));  // sweeps 99 (id3), then 100: id1 fully, id2 partially
  auto& ev = SINK(b);
  CHECK_EQ(ev.size(), 4u);  // Accepted + 3 fills
  CHECK(ev[1].type == EvType::Fill && ev[1].other == 3 && ev[1].price == 99 && ev[1].qty == 5);
  CHECK(ev[2].type == EvType::Fill && ev[2].other == 1 && ev[2].price == 100 && ev[2].qty == 5);
  CHECK(ev[3].type == EvType::Fill && ev[3].other == 2 && ev[3].price == 100 && ev[3].qty == 2);
  CHECK_EQ(b.level_qty(Side::Sell, 100), 3u);
  CHECK_EQ(b.best_ask(), 100);
  CHECK_EQ(b.best_bid(), -1);  // buyer fully filled, nothing rests
  CHECK_MSG(b.validate().empty(), b.validate());
}

TEST(book_partial_fill_rests_remainder) {
  Book b(cfg());
  b.process(sell(1, 100, 4));
  b.process(buy(2, 100, 10));
  CHECK_EQ(b.best_bid(), 100);
  CHECK_EQ(b.level_qty(Side::Buy, 100), 6u);
  CHECK_EQ(b.best_ask(), -1);
  CHECK_MSG(b.validate().empty(), b.validate());
}

TEST(book_best_advances_when_level_exhausted) {
  Book b(cfg());
  for (int i = 0; i < 5; ++i) b.process(sell(1 + i, 100 + 10 * i, 1));
  b.process(buy(99, 130, 3));  // eats 100, 110, 120
  CHECK_EQ(b.best_ask(), 130);
  b.process(cancel(4));
  CHECK_EQ(b.best_ask(), 140);
  b.process(cancel(5));
  CHECK_EQ(b.best_ask(), -1);
  CHECK_MSG(b.validate().empty(), b.validate());
}

TEST(book_cancel_and_unknown) {
  Book b(cfg());
  b.process(buy(1, 50, 9));
  SINK(b).clear();
  b.process(cancel(1));
  CHECK(SINK(b).size() == 1 && SINK(b)[0].type == EvType::Cancelled && SINK(b)[0].qty == 9 && SINK(b)[0].price == 50);
  b.process(cancel(1));  // already gone
  CHECK(SINK(b)[1].type == EvType::Rejected && SINK(b)[1].reason == Reject::UnknownId);
  CHECK_EQ(b.live_orders(), 0u);
  CHECK_EQ(b.best_bid(), -1);
}

TEST(book_ioc_cancels_remainder) {
  Book b(cfg());
  b.process(sell(1, 100, 4));
  SINK(b).clear();
  b.process(msg(MsgType::NewIOC, 2, Side::Buy, 100, 10));
  auto& ev = SINK(b);
  CHECK_EQ(ev.size(), 3u);  // Accepted, Fill 4, Cancelled 6
  CHECK(ev[2].type == EvType::Cancelled && ev[2].qty == 6);
  CHECK_EQ(b.live_orders(), 0u);
}

TEST(book_validation_rejects) {
  Book b(cfg(10, 20, 3));
  auto last = [&] { return SINK(b).back(); };
  b.process(buy(0, 15, 1));   CHECK(last().reason == Reject::BadId);
  b.process(buy(1, 15, 0));   CHECK(last().reason == Reject::BadQty);
  b.process(buy(1, 9, 1));    CHECK(last().reason == Reject::BadPrice);
  b.process(buy(1, 20, 1));   CHECK(last().reason == Reject::BadPrice);  // max is exclusive
  b.process(buy(1, 10, 1));   CHECK(last().type == EvType::Accepted);
  b.process(buy(1, 11, 1));   CHECK(last().reason == Reject::DuplicateId);
  b.process(buy(2, 11, 1));
  b.process(buy(3, 12, 1));
  b.process(buy(4, 13, 1));   CHECK(last().reason == Reject::PoolFull);
  CHECK_EQ(b.live_orders(), 3u);
  CHECK_MSG(b.validate().empty(), b.validate());
}

TEST(book_modify_in_place_keeps_priority) {
  Book b(cfg());
  b.process(sell(1, 100, 10));
  b.process(sell(2, 100, 10));
  b.process(modify(1, 100, 4));  // reduce: id1 keeps first place
  b.process(buy(9, 100, 4));
  auto f = fills(b);
  CHECK_EQ(f.size(), 1u);
  CHECK(f[0].other == 1 && f[0].qty == 4);
  CHECK_MSG(b.validate().empty(), b.validate());
}

TEST(book_modify_replace_loses_priority_and_can_cross) {
  Book b(cfg());
  b.process(sell(1, 100, 5));
  b.process(sell(2, 100, 5));
  b.process(modify(1, 100, 8));  // increase qty: replace, goes to back of queue
  SINK(b).clear();
  b.process(buy(9, 100, 5));
  CHECK(SINK(b)[1].type == EvType::Fill && SINK(b)[1].other == 2);  // id2 is now first
  // replace to a crossing price: the modified order trades
  Book c(cfg());
  c.process(sell(1, 100, 5));
  c.process(buy(2, 90, 5));
  SINK(c).clear();
  c.process(modify(2, 100, 5));
  CHECK(SINK(c)[0].type == EvType::Modified);
  CHECK(SINK(c)[1].type == EvType::Fill && SINK(c)[1].qty == 5);
  CHECK_EQ(c.live_orders(), 0u);
  CHECK_MSG(b.validate().empty() && c.validate().empty(), b.validate() + c.validate());
}

TEST(book_modify_rejects_without_side_effects) {
  Book b(cfg(0, 100, 10));
  b.process(buy(1, 50, 5));
  SINK(b).clear();
  b.process(modify(1, 50, 0));    CHECK(SINK(b).back().reason == Reject::BadQty);
  b.process(modify(1, 100, 5));   CHECK(SINK(b).back().reason == Reject::BadPrice);
  b.process(modify(7, 50, 5));    CHECK(SINK(b).back().reason == Reject::UnknownId);
  CHECK_EQ(b.level_qty(Side::Buy, 50), 5u);
  CHECK_MSG(b.validate().empty(), b.validate());
}

TEST(book_pool_slots_are_recycled) {
  Book b(cfg(0, 1000, 8));
  for (int round = 0; round < 1000; ++round) {
    for (int i = 0; i < 8; ++i) b.process(buy(round * 8 + i + 1, 500, 1));
    for (int i = 0; i < 8; ++i) b.process(cancel(round * 8 + i + 1));
  }
  CHECK_EQ(b.live_orders(), 0u);
  CHECK_MSG(b.validate().empty(), b.validate());
}
