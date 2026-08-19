#pragma once
// Forward declarations for the risk module, and the one place that says what
// its public vocabulary is called.
//
// Risk sits *between* the strategy host and the execution gateway, and it gets
// there without either of them naming it: it models the same structural
// `command_sink` the host already writes through, so a gate wraps a partition
// and the host cannot tell the difference. There is therefore no edge from
// strategy/ to risk/ and none from risk/ to strategy/ - the conformance is
// checked by a static_assert in the test tree, which is allowed to name both.

#include "hooks/pre_trade/fwd.hpp"           // IWYU pragma: export
#include "hooks/system/fwd.hpp"              // IWYU pragma: export
#include "risk_management_export.hpp"        // IWYU pragma: export
#include "trading-engine/order_book/fwd.hpp" // IWYU pragma: export
#include "trading-engine/orders/fwd.hpp"     // IWYU pragma: export

#include <cstdint>

namespace exchange::risk {

// Declarations only - no dll interface on any of them. Exporting a
// non-polymorphic class wholesale makes MSVC treat its inline members as ABI
// and makes each static constexpr member an imported object no translation unit
// defines, which MinGW reports as an unresolved `__imp_` reference. The
// annotation goes on the out-of-line members instead, where they are declared.
struct risk_limits;

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

// --- the flat vocabulary --------------------------------------------------
//
// Each of these lives in the directory of the hook that owns it - the ledger
// with the duplicate rule, the breaker with the kill switch - because a hook
// should not have to include upward to reach the state it is about. That is a
// statement about *filing*, and it should not be a statement about what a
// caller has to type: a deployment wiring a gate says `risk::circuit_breaker`
// and has no business knowing which of the eight hooks happens to own it.
//
// So each of those headers ends with a using-declaration putting its own type
// back into `exchange::risk`, flat, and that set is the module's public
// surface. The declaration sits beside the definition rather than here because
// a consumer includes one header for the type it wants; needing a second one to
// learn the type's short name would defeat the point of having a short name.
// Moving a component between hook directories then costs one line in this file
// and nothing at any call site, which is the whole point of having it.
} // namespace exchange::risk
