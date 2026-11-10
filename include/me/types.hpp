#pragma once
#include <cstdint>

namespace me {

using OrderId = std::uint64_t;  // 0 is reserved (invalid)
using Price = std::int32_t;     // integer ticks
using Qty = std::uint32_t;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };
enum class MsgType : std::uint8_t { New, NewIOC, Cancel, Modify };

// Inbound message. `ts` is stamped by the producer (TSC cycles) for end-to-end latency measurement.
struct alignas(32) Msg {
  std::uint64_t ts;
  OrderId id;
  Price price;
  Qty qty;
  Side side;
  MsgType type;
};
static_assert(sizeof(Msg) == 32);

enum class EvType : std::uint8_t { Accepted, Fill, Cancelled, Modified, Rejected };
enum class Reject : std::uint8_t { None, BadId, BadQty, BadPrice, DuplicateId, PoolFull, UnknownId };

// Outbound event.
//   Accepted : id, side, price, qty (original)
//   Fill     : id = taker, other = maker, side = taker side, price = maker's resting price, qty = fill size
//   Cancelled: id, side, price, qty = remaining quantity removed (explicit cancel, or IOC remainder)
//   Modified : id, side, price/qty = new values
//   Rejected : id, reason (+ echo of the message fields)
struct Event {
  EvType type;
  Reject reason;
  Side side;
  OrderId id;
  OrderId other;
  Price price;
  Qty qty;
  bool operator==(const Event&) const = default;
};

inline Event make_event(EvType t, Side s, OrderId id, OrderId other, Price p, Qty q, Reject r = Reject::None) {
  return Event{t, r, s, id, other, p, q};
}

}  // namespace me
