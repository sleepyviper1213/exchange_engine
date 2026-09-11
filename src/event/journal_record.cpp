#include "journal_record.hpp"

#include "orders/order.hpp"
#include "orders/order_type.hpp"
#include "orders/side.hpp"
#include "orders/time_in_force_instruction.hpp"
#include "orders/types.hpp"

#include <fmt/format.h>

#include <bit>
#include <cstring>

namespace exchange::engine::event {
namespace {

using orders::order;
using orders::order_type;
using orders::time_in_force_instruction;

// The layout, once. Every offset below is the format's, not an implementation
// detail - see the table on journal_record.
constexpr std::size_t AT_TAG        = 0;
constexpr std::size_t AT_SYMBOL     = 1;
constexpr std::size_t AT_ID         = 5;
constexpr std::size_t AT_SYMBOL_ID  = 13;
constexpr std::size_t AT_SIDE       = 17;
constexpr std::size_t AT_ORDER_TYPE = 18;
constexpr std::size_t AT_TIF        = 19;
constexpr std::size_t AT_PRICE      = 20;
constexpr std::size_t AT_STOP_PRICE = 24;
constexpr std::size_t AT_QUANTITY   = 28;
constexpr std::size_t AT_TIMESTAMP  = 32;

static_assert(AT_TIMESTAMP + sizeof(std::uint64_t) == journal_record::SIZE,
			  "the layout does not fill the record it is the layout of");

/// @brief Store @p value at @p at, little-endian.
///
/// Through @c bit_cast of the value rather than a cast of the pointer: there is
/// no object of @p T at @p at yet, so only the value-to-bytes direction is
/// defined. Folds to one unaligned store on every target here.
template <class T>
void store_le(std::byte *at, T value) noexcept {
	static_assert(std::is_trivially_copyable_v<T>);
	if constexpr (std::endian::native == std::endian::big)
		value = std::byteswap(value);
	const auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
	std::memcpy(at, bytes.data(), bytes.size());
}

/// @brief Load a little-endian @p T from @p at.
template <class T>
[[nodiscard]] T load_le(const std::byte *at) noexcept {
	static_assert(std::is_trivially_copyable_v<T>);
	T value{};
	std::memcpy(&value, at, sizeof value);
	if constexpr (std::endian::native == std::endian::big)
		return std::byteswap(value);
	return value;
}

/// @brief The raw byte at @p offset.
[[nodiscard]] std::uint8_t byte_at(const journal_record &record,
								   std::size_t offset) noexcept {
	return std::to_integer<std::uint8_t>(record.bytes[offset]);
}

/// @brief @p byte as a @c side_t, or nothing if it names neither side.
///
/// The check that cannot be skipped. @c side_t has a @c bool underlying type,
/// so its value representation is {0, 1} and a cast of anything else is
/// undefined behaviour - not a wrong answer that a later range check could
/// catch, but a program that is no longer meaningful. So the *byte* is tested,
/// before the cast, and every other enum below can be checked the ordinary way
/// afterwards.
[[nodiscard]] std::optional<side_t> to_side(std::uint8_t byte) noexcept {
	if (byte > 1U) return std::nullopt;
	return static_cast<side_t>(byte != 0U);
}

/// @brief Whether @p tag names one of the four command types.
[[nodiscard]] bool is_known_tag(std::uint8_t tag) noexcept {
	switch (static_cast<command_type>(tag)) {
	case command_type::PLACE:
	case command_type::CANCEL:
	case command_type::ADD:
	case command_type::REDUCE: return true;
	default: return false;
	}
}

/// @brief Decode the PLACE arm, whose payload is a whole order.
[[nodiscard]] std::expected<command, std::string>
decode_place(const journal_record &record) {
	const std::optional<side_t> side = to_side(byte_at(record, AT_SIDE));
	if (!side)
		return std::unexpected(
			fmt::format("side byte {} names neither side of the book",
						byte_at(record, AT_SIDE)));

	// These two are uint8_t-backed, so any byte casts legitimately and the
	// generated to_string is what says whether the result means anything -
	// an empty name is the enum machinery's own "out of range". @see
	// enum_string.hpp
	const auto type = static_cast<order_type>(byte_at(record, AT_ORDER_TYPE));
	if (orders::to_string(type).empty())
		return std::unexpected(
			fmt::format("order type {} is not one this build knows",
						byte_at(record, AT_ORDER_TYPE)));

	const auto tif =
		static_cast<time_in_force_instruction>(byte_at(record, AT_TIF));
	if (orders::to_string(tif).empty())
		return std::unexpected(
			fmt::format("time in force {} is not one this build knows",
						byte_at(record, AT_TIF)));

	const order restored{
		.id         = load_le<order_id_t>(record.bytes.data() + AT_ID),
		.symbol_id  = load_le<symbol_id_t>(record.bytes.data() + AT_SYMBOL_ID),
		.side       = *side,
		.type       = type,
		.tif        = tif,
		.price      = load_le<price_t>(record.bytes.data() + AT_PRICE),
		.stop_price = load_le<price_t>(record.bytes.data() + AT_STOP_PRICE),
		.qty        = load_le<quantity_t>(record.bytes.data() + AT_QUANTITY),
		.timestamp = load_le<std::uint64_t>(record.bytes.data() + AT_TIMESTAMP),
	};

	// Built through the factory rather than by assembling the union
	// directly, so the tag-matches-payload invariant is the one command
	// already guarantees instead of one this file would have to reproduce.
	const command rebuilt = command::place(restored);

	// The factory takes the routing symbol from the order, so the two
	// fields must agree on the way back in - and if they do not, this
	// record did not come from a command. Checking it here is what keeps a
	// silently mis-routed replay from being the way that gets discovered.
	if (const auto symbol =
			load_le<symbol_id_t>(record.bytes.data() + AT_SYMBOL);
		rebuilt.symbol != symbol)
		return std::unexpected(
			fmt::format("a PLACE record routes to symbol {} but carries an "
						"order for {}",
						symbol,
						rebuilt.symbol));

	return rebuilt;
}

/// @brief Decode an ADD or REDUCE, whose payload is a side, a price and a
/// size.
[[nodiscard]] std::expected<command, std::string>
decode_level(const journal_record &record, command_type tag) {
	const std::optional<side_t> side = to_side(byte_at(record, AT_SIDE));
	if (!side)
		return std::unexpected(
			fmt::format("side byte {} names neither side of the book",
						byte_at(record, AT_SIDE)));

	const auto symbol = load_le<symbol_id_t>(record.bytes.data() + AT_SYMBOL);
	const auto price  = load_le<price_t>(record.bytes.data() + AT_PRICE);
	const auto volume = load_le<quantity_t>(record.bytes.data() + AT_QUANTITY);

	// The bytes are proven to be the ones written; nothing yet proves they
	// are a command. A depth command's size is positive by construction on
	// both producers - the bridge emits a strictly positive difference and
	// the seed helpers a real size - so a non-positive one here is a record
	// this engine did not write, and it is refused rather than replayed.
	//
	// This is not a defence in depth: it is the only check on the path. A
	// PLACE carrying a bad quantity is caught by
	// order_book::reject_if_invalid, which answers it with
	// NON_POSITIVE_QUANTITY. ADD has no such boundary - it reaches
	// order_book::add_order, whose terminus is an order_state whose
	// constructor documents that the validation boundary must reject one
	// before it ever gets here. On this path this is that boundary.
	if (volume <= 0)
		return std::unexpected(
			fmt::format("a {} record carries a size of {}, which is not a "
						"size this engine ever wrote",
						tag == command_type::ADD ? "an ADD" : "a REDUCE",
						volume));

	return tag == command_type::ADD
			   ? command::add(symbol, *side, price, volume)
			   : command::reduce(symbol, *side, price, volume);
}

} // namespace

journal_record encode(const command &cmd) noexcept {
	// Zeroed, so the bytes an arm does not use are zero rather than whatever
	// the stack held - which is what makes encoding a command twice give the
	// same record. @see journal_record
	journal_record record{};
	std::byte *at = record.bytes.data();

	at[AT_TAG] = static_cast<std::byte>(cmd.type);
	store_le<symbol_id_t>(at + AT_SYMBOL, cmd.symbol);

	switch (cmd.type) {
	case command_type::PLACE: {
		const order &o = cmd.as_place();
		store_le<order_id_t>(at + AT_ID, o.id);
		store_le<symbol_id_t>(at + AT_SYMBOL_ID, o.symbol_id);
		at[AT_SIDE]       = static_cast<std::byte>(o.side);
		at[AT_ORDER_TYPE] = static_cast<std::byte>(o.type);
		at[AT_TIF]        = static_cast<std::byte>(o.tif);
		store_le<price_t>(at + AT_PRICE, o.price);
		store_le<price_t>(at + AT_STOP_PRICE, o.stop_price);
		store_le<quantity_t>(at + AT_QUANTITY, o.qty);
		store_le<std::uint64_t>(at + AT_TIMESTAMP, o.timestamp);
		break;
	}
	case command_type::CANCEL:
		store_le<order_id_t>(at + AT_ID, cmd.as_cancel());
		break;
	case command_type::ADD:
	case command_type::REDUCE: {
		const level_change &level = cmd.as_level();
		at[AT_SIDE]               = static_cast<std::byte>(level.side);
		store_le<price_t>(at + AT_PRICE, level.price);
		store_le<quantity_t>(at + AT_QUANTITY, level.volume);
		break;
	}
	}
	return record;
}

std::expected<command, std::string>
decode(const journal_record &record) noexcept try {
	const std::uint8_t tag = byte_at(record, AT_TAG);
	if (!is_known_tag(tag))
		return std::unexpected(
			fmt::format("{} is not a command type this build knows", tag));

	switch (static_cast<command_type>(tag)) {
	case command_type::PLACE: return decode_place(record);
	case command_type::CANCEL:
		return command::cancel(
			load_le<symbol_id_t>(record.bytes.data() + AT_SYMBOL),
			load_le<order_id_t>(record.bytes.data() + AT_ID));
	case command_type::ADD:
	case command_type::REDUCE:
		return decode_level(record, static_cast<command_type>(tag));
	}
	// Unreachable: is_known_tag above admits exactly the four cases the switch
	// covers. Spelled rather than asserted because a function returning
	// expected has to return something on every path a compiler can see.
	return std::unexpected(std::string("unreachable command type"));
} catch (const std::bad_alloc &) {
	// The only thing here that can throw is building an error message, and this
	// is declared noexcept because a decode failure must not become a terminate
	// on the recovery path. A record that could not even be complained about is
	// still a record that was refused.
	return std::unexpected(std::string("out of memory decoding a record"));
}

} // namespace exchange::engine::event
