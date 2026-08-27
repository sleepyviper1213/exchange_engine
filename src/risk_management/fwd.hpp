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

namespace exchange::risk {

// Declarations only - no dll interface on any of them. Exporting a
// non-polymorphic class wholesale makes MSVC treat its inline members as ABI
// and makes each static constexpr member an imported object no translation unit
// defines, which MinGW reports as an unresolved `__imp_` reference. The
// annotation goes on the out-of-line members instead, where they are declared.
struct risk_limits;

// risk_gate is deliberately absent, for the same reason strategy_engine is: its
// clock parameter is constrained, and a declaration that drops the constraint
// declares a different template rather than referring to this one. Naming the
// gate means including gate.hpp, which is the weight this header avoids.
} // namespace exchange::risk
