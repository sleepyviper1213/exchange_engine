#pragma once

// The order vocabulary the book is built on lives one module over, in
// exchange::engine::orders - plural, because its principal type is `order`.
#include "trading-engine/orders/fwd.hpp" // IWYU pragma: export

#include <cstdint>

namespace exchange::engine {


enum class OrderStatus : std::uint8_t;
enum class OutcomeType : std::uint8_t;
enum class reject_reason : std::uint8_t;

// No class here carries a dll interface, and that is deliberate. Exporting a
// non-polymorphic class wholesale makes MSVC treat its *inline* members as part
// of the ABI - they stop being inlined across the boundary - and it makes every
// static constexpr member an imported object that no translation unit defines,
// which MinGW reports as an unresolved `__imp_` reference. So the annotation
// goes on the out-of-line public members instead, in the header that declares
// them. @see the Qt wiki's binary-compatibility rules.
struct price_level;
struct trade;
struct order_outcome;
class order_state;
class order_book;

} // namespace exchange::engine
