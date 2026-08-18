#pragma once
// Forward declarations for the risk submodule.
//
// Risk sits *between* the strategy host and the execution gateway, and it gets
// there without either of them naming it: it models the same structural
// `command_sink` the host already writes through, so a gate wraps a partition
// and the host cannot tell the difference. There is therefore no edge from
// strategy/ to risk/ and none from risk/ to strategy/ - the conformance is
// checked by a static_assert in the test tree, which is allowed to name both.

#include "risk_management_export.hpp" // RISK_MANAGEMENT_EXPORT (generated)
#include "trading-engine/order_book/fwd.hpp" // IWYU pragma: export
#include "trading-engine/orders/fwd.hpp"     // IWYU pragma: export

#include <cstdint>

namespace exchange::risk {

// Declarations only - no dll interface on any of them. Exporting a
// non-polymorphic class wholesale makes MSVC treat its inline members as ABI
// and makes each static constexpr member an imported object no translation unit
// defines, which MinGW reports as an unresolved `__imp_` reference. The
// annotation goes on the out-of-line members instead, where they are declared.
//
// One thing that survived from the wholesale attempt: those members are marked
// RISK_MANAGEMENT_EXPORT, not the AUTOTEST variant they used to carry. AUTOTEST
// resolves to *no* export unless ORDER_BOOK_BUILD_TESTS is set, so a shipping
// build left `risk_limits::has_loss_limit` and `position_snapshot::pnl` out of
// the import library - symbols the gate calls from a header, in every consumer.
// It went unnoticed because a top-level build turns tests on.
struct risk_limits;
struct position_snapshot;
struct working_order;
struct ledger_take;

enum class breach : std::uint16_t;
enum class trading_state : std::uint8_t;
enum class trip_cause : std::uint8_t;

class position_book;
class rate_limiter;
class circuit_breaker;
class working_ledger;

// No dll interface, and this one is not a style choice: steady_nanos is a
// single inline member wrapping steady_clock::now, and no translation unit
// inside this module includes clock.hpp. Exporting it makes every consumer
// import a symbol the library never emits, which the linker reports against
// whichever gate instantiation happened to need it.
struct steady_nanos;

// risk_gate is deliberately absent, for the same reason strategy_engine is: its
// clock parameter is constrained, and a declaration that drops the constraint
// declares a different template rather than referring to this one. Naming the
// gate means including gate.hpp, which is the weight this header avoids.

} // namespace exchange::risk
