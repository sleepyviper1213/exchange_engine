#pragma once
// Forward declarations for the risk submodule.
//
// Risk sits *between* the strategy host and the execution gateway, and it gets
// there without either of them naming it: it models the same structural
// `command_sink` the host already writes through, so a gate wraps a partition
// and the host cannot tell the difference. There is therefore no edge from
// strategy/ to risk/ and none from risk/ to strategy/ — the conformance is
// checked by a static_assert in the test tree, which is allowed to name both.

#include "trading-engine/order_book/fwd.hpp" // IWYU pragma: export
#include "trading-engine/orders/fwd.hpp"     // IWYU pragma: export

#include <cstdint>

namespace exchange::risk {

enum class breach : std::uint16_t;
enum class trading_state : std::uint8_t;

struct risk_limits;
struct position_snapshot;
struct working_order;
struct ledger_take;

class position_book;
class rate_limiter;
class circuit_breaker;
class working_ledger;

struct steady_nanos;

// risk_gate is deliberately absent, for the same reason strategy_engine is: its
// clock parameter is constrained, and a declaration that drops the constraint
// declares a different template rather than referring to this one. Naming the
// gate means including gate.hpp, which is the weight this header avoids.

} // namespace exchange::risk
