#pragma once

#include "trading_engine_export.hpp"
namespace exchange::engine::event {
// No class here carries a dll interface, and that is deliberate. Exporting a
// non-polymorphic class wholesale makes MSVC treat its *inline* members as part
// of the ABI — they stop being inlined across the boundary — and it makes every
// static constexpr member an imported object that no translation unit defines,
// which MinGW reports as an unresolved `__imp_` reference. So the annotation
// goes on the out-of-line public members instead, in the header that declares
// them. @see the Qt wiki's binary-compatibility rules.
struct level_change;
struct command;
} // namespace exchange::engine::event