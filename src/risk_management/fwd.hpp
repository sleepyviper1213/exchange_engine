#pragma once
// Forward declarations for the risk submodule.
//
// Risk sits *between* the strategy host and the execution gateway, and it gets
// there without either of them naming it: it models the same structural
// `command_sink` the host already writes through, so a gate wraps a partition
// and the host cannot tell the difference. There is therefore no edge from
// strategy/ to risk/ and none from risk/ to strategy/ — the conformance is
// checked by a static_assert in the test tree, which is allowed to name both.

#include "risk_management_export.hpp"
#include "trading-engine/order_book/fwd.hpp" // IWYU pragma: export
#include "trading-engine/orders/fwd.hpp"     // IWYU pragma: export

#include <cstdint>

namespace exchange::risk {

struct RISK_MANAGEMENT_AUTOTEST_EXPORT risk_limits;
struct RISK_MANAGEMENT_AUTOTEST_EXPORT position_snapshot;
struct RISK_MANAGEMENT_AUTOTEST_EXPORT working_order;
struct RISK_MANAGEMENT_AUTOTEST_EXPORT ledger_take;

enum class RISK_MANAGEMENT_EXPORT breach : std::uint16_t;
enum class RISK_MANAGEMENT_EXPORT trading_state : std::uint8_t;

class RISK_MANAGEMENT_EXPORT position_book;
class RISK_MANAGEMENT_EXPORT rate_limiter;
class RISK_MANAGEMENT_EXPORT circuit_breaker;
class RISK_MANAGEMENT_EXPORT working_ledger;

struct RISK_MANAGEMENT_AUTOTEST_EXPORT steady_nanos;

// risk_gate is deliberately absent, for the same reason strategy_engine is: its
// clock parameter is constrained, and a declaration that drops the constraint
// declares a different template rather than referring to this one. Naming the
// gate means including gate.hpp, which is the weight this header avoids.

} // namespace exchange::risk
