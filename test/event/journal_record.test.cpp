#include "event/command.hpp"
#include "event/journal_record.hpp"
#include "orders/order.hpp"
#include "orders/order_type.hpp"
#include "orders/side.hpp"
#include "orders/time_in_force_instruction.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

// The journal's on-disk encoding. Two properties carry it and they pull in
// opposite directions, which is why both need pinning:
//
//   - Every command that can be built round-trips through the bytes unchanged.
//     A field silently dropped here is a field a recovered book is missing, and
//     nothing downstream would notice - the replay would simply produce a
//     different, plausible book.
//   - Not every sequence of bytes is a command. The record log guarantees the
//     bytes are the ones that were written; it cannot guarantee they mean
//     anything, so decode has to refuse what it cannot interpret rather than
//     manufacture a command nobody asked for.
//
// The layout itself is checked by byte offset on purpose. A round-trip test
// alone would pass just as happily if encode and decode agreed on a layout that
// was not the documented one, and the documented one is the format - it is what
// a reader on another machine, or a build five years from now, has to match.

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::event;
using namespace exchange::engine::orders;

namespace {

/// @brief A fully-populated PLACE: every field non-zero and distinct, so a
///        round trip that crossed two of them over shows up as a mismatch
///        rather than as two fields that happened to be equal.
[[nodiscard]] command populated_place() {
	return command::place({.id         = 0x0102030405060708ULL,
						   .symbol_id  = 0x11223344U,
						   .side       = side_t::ask,
						   .type       = order_type::MARKET,
						   .tif        = time_in_force_instruction::FILL_OR_KILL,
						   .price      = 0x55667788U,
						   .stop_price = 0x99AABBCCU,
						   .qty        = 0x0D0E0F10,
						   .timestamp  = 0xF0E0D0C0B0A09080ULL});
}

/// @brief The byte at @p offset of @p record.
[[nodiscard]] std::uint8_t at(const journal_record &record,
							  std::size_t offset) {
	return std::to_integer<std::uint8_t>(record.bytes[offset]);
}

/// @brief The little-endian value of @p width bytes starting at @p offset.
[[nodiscard]] std::uint64_t
field(const journal_record &record, std::size_t offset, std::size_t width) {
	std::uint64_t value = 0;
	for (std::size_t i = 0; i < width; ++i)
		value |= static_cast<std::uint64_t>(at(record, offset + i)) << (8U * i);
	return value;
}

TEST(JournalRecord, APlaceRoundTripsEveryField) {
	const command original = populated_place();
	const auto restored    = decode(encode(original));

	ASSERT_TRUE(restored.has_value()) << restored.error();
	EXPECT_EQ(restored->type, command_type::PLACE);
	EXPECT_EQ(restored->symbol, original.symbol);
	// order's operator== is field-by-field including the timestamp, so this is
	// the whole payload and not a sample of it.
	EXPECT_EQ(restored->as_place(), original.as_place());
}

TEST(JournalRecord, ACancelRoundTrips) {
	const command original = command::cancel(0xDEADBEEFU, 0x0102030405060708ULL);
	const auto restored    = decode(encode(original));

	ASSERT_TRUE(restored.has_value()) << restored.error();
	EXPECT_EQ(restored->type, command_type::CANCEL);
	EXPECT_EQ(restored->symbol, original.symbol);
	EXPECT_EQ(restored->as_cancel(), original.as_cancel());
}

TEST(JournalRecord, AnAddRoundTrips) {
	const command original = command::add(7, side_t::bid, 1234, 56);
	const auto restored    = decode(encode(original));

	ASSERT_TRUE(restored.has_value()) << restored.error();
	EXPECT_EQ(restored->type, command_type::ADD);
	EXPECT_EQ(restored->symbol, original.symbol);
	EXPECT_EQ(restored->as_level().side, side_t::bid);
	EXPECT_EQ(restored->as_level().price, 1234U);
	EXPECT_EQ(restored->as_level().volume, 56);
}

// REDUCE shares its payload and every byte offset with ADD, so the tag is the
// only thing separating them - which makes it exactly the thing worth checking.
TEST(JournalRecord, AReduceRoundTripsAndStaysDistinctFromAnAdd) {
	const command reduce = command::reduce(7, side_t::ask, 1234, 56);
	const command add    = command::add(7, side_t::ask, 1234, 56);

	const auto restored = decode(encode(reduce));
	ASSERT_TRUE(restored.has_value()) << restored.error();
	EXPECT_EQ(restored->type, command_type::REDUCE);
	EXPECT_EQ(restored->as_level().volume, 56);

	EXPECT_NE(encode(reduce), encode(add)) << "the tag did not reach the bytes";
}

// The extremes of every width. A field encoded as the wrong number of bytes
// survives typical values and fails here.
//
// A REDUCE carries a *magnitude*, not a signed delta - the direction is the tag,
// which is what AReduceRoundTripsAndStaysDistinctFromAnAdd pins - so the widest
// depth size is quantity_t's maximum and not its minimum. The signedness of the
// field is pinned by ANonPositiveDepthSizeIsRefused below, which is a stronger
// check than a round trip could be: a negative size read back through an
// unsigned type would come out large and positive, and be accepted.
TEST(JournalRecord, ExtremeValuesSurvive) {
	const command widest_level =
		command::reduce(std::numeric_limits<symbol_id_t>::max(),
						side_t::bid,
						std::numeric_limits<price_t>::max(),
						std::numeric_limits<quantity_t>::max());
	const auto restored = decode(encode(widest_level));
	ASSERT_TRUE(restored.has_value()) << restored.error();
	EXPECT_EQ(restored->symbol, std::numeric_limits<symbol_id_t>::max());
	EXPECT_EQ(restored->as_level().price, std::numeric_limits<price_t>::max());
	EXPECT_EQ(restored->as_level().volume,
			  std::numeric_limits<quantity_t>::max());

	const command widest = command::place(
		{.id        = std::numeric_limits<order_id_t>::max(),
		 .symbol_id = std::numeric_limits<symbol_id_t>::max(),
		 .side      = side_t::ask,
		 .price     = std::numeric_limits<price_t>::max(),
		 .qty       = std::numeric_limits<quantity_t>::max(),
		 .timestamp = std::numeric_limits<std::uint64_t>::max()});
	const auto back = decode(encode(widest));
	ASSERT_TRUE(back.has_value()) << back.error();
	EXPECT_EQ(back->as_place(), widest.as_place());
}

// The format, by offset. This is the suite that fails if somebody "tidies" the
// layout - which is the change that silently invalidates every journal on disk.
TEST(JournalRecord, TheLayoutIsTheDocumentedOne) {
	static_assert(journal_record::SIZE == 40);
	const journal_record record = encode(populated_place());

	EXPECT_EQ(at(record, 0), static_cast<std::uint8_t>(command_type::PLACE));
	EXPECT_EQ(field(record, 1, 4), 0x11223344ULL);          // symbol
	EXPECT_EQ(field(record, 5, 8), 0x0102030405060708ULL);  // id
	EXPECT_EQ(field(record, 13, 4), 0x11223344ULL);         // symbol_id
	EXPECT_EQ(at(record, 17), 1U);                          // side: ask
	EXPECT_EQ(at(record, 18),
			  static_cast<std::uint8_t>(order_type::MARKET));
	EXPECT_EQ(at(record, 19),
			  static_cast<std::uint8_t>(time_in_force_instruction::FILL_OR_KILL));
	EXPECT_EQ(field(record, 20, 4), 0x55667788ULL);         // price
	EXPECT_EQ(field(record, 24, 4), 0x99AABBCCULL);         // stop_price
	EXPECT_EQ(field(record, 28, 4), 0x0D0E0F10ULL);         // qty
	EXPECT_EQ(field(record, 32, 8), 0xF0E0D0C0B0A09080ULL); // timestamp
}

// Little-endian explicitly, not "whatever this machine does". The test is only
// meaningful because it names a byte: on a big-endian host the encoder byte-swaps
// and this still passes, which is the entire point of having a defined order.
TEST(JournalRecord, MultiByteFieldsAreLittleEndian) {
	const journal_record record =
		encode(command::cancel(0x04030201U, 0x0807060504030201ULL));

	EXPECT_EQ(at(record, 1), 0x01U) << "symbol's least significant byte first";
	EXPECT_EQ(at(record, 4), 0x04U) << "and its most significant byte last";
	EXPECT_EQ(at(record, 5), 0x01U) << "id's least significant byte first";
	EXPECT_EQ(at(record, 12), 0x08U) << "and its most significant byte last";
}

// Encoding is deterministic: the bytes an arm does not use are zero rather than
// whatever was on the stack. Without this a record's checksum would vary between
// two encodings of the same command, and two journals of the same flow would not
// compare equal.
TEST(JournalRecord, UnusedBytesAreZeroedSoEncodingIsDeterministic) {
	const command cancel = command::cancel(9, 42);
	EXPECT_EQ(encode(cancel), encode(cancel));

	const journal_record record = encode(cancel);
	// A CANCEL uses the tag, the symbol and the id - bytes 0 through 12. The
	// rest belong to the PLACE arm and must be zero here.
	for (std::size_t offset = 13; offset < journal_record::SIZE; ++offset)
		EXPECT_EQ(at(record, offset), 0U) << "byte " << offset << " is not zero";
}

TEST(JournalRecord, AnUnknownTagIsRefused) {
	journal_record record{};
	record.bytes[0] = std::byte{99};

	const auto restored = decode(record);
	ASSERT_FALSE(restored.has_value()) << "decoded a command type that does not exist";
	EXPECT_TRUE(restored.error().contains("command type")) << restored.error();
}

// The one that is undefined behaviour rather than a wrong answer: side_t has a
// bool underlying type, so its value representation is {0, 1} and casting a 2 to
// it is not a strange side, it is a program that has stopped meaning anything.
// The byte is therefore checked before the cast.
TEST(JournalRecord, ASideByteOutsideZeroAndOneIsRefused) {
	for (const std::uint8_t bogus : {std::uint8_t{2}, std::uint8_t{0xFF}}) {
		journal_record place = encode(populated_place());
		place.bytes[17]      = static_cast<std::byte>(bogus);
		const auto decoded_place = decode(place);
		EXPECT_FALSE(decoded_place.has_value())
			<< "accepted side byte " << static_cast<int>(bogus) << " on a PLACE";

		journal_record level = encode(command::add(1, side_t::bid, 10, 5));
		level.bytes[17]      = static_cast<std::byte>(bogus);
		const auto decoded_level = decode(level);
		EXPECT_FALSE(decoded_level.has_value())
			<< "accepted side byte " << static_cast<int>(bogus) << " on an ADD";
	}
}

// A depth size of zero or less is not a command this engine ever wrote: the
// bridge emits a strictly positive difference and the seed helpers a real size.
// It matters more than the other refusals because ADD is the one command with
// no validation waiting for it downstream - a PLACE carrying a bad quantity is
// answered by order_book::reject_if_invalid, while an ADD runs to
// order_book::add_order and then to an order_state whose only guard against a
// non-positive quantity is an assertion that an optimised build removes. What
// would be left is the raw store of a negative value into a packed 31-bit
// field: a resting order of some two-billion-lot size, already flagged
// cancelled. So this is the boundary, and it is the only one on the path.
//
// The negative case also pins the field's signedness. Decoded through an
// unsigned type, quantity_t's minimum reads back as 2147483648 - large,
// positive, and accepted - so a refusal here is proof the load is signed.
TEST(JournalRecord, ANonPositiveDepthSizeIsRefused) {
	for (const quantity_t bogus : {quantity_t{0},
								   quantity_t{-1},
								   std::numeric_limits<quantity_t>::min()}) {
		const auto add = decode(encode(command::add(1, side_t::bid, 10, bogus)));
		ASSERT_FALSE(add.has_value())
			<< "decoded an ADD of size " << bogus;
		EXPECT_TRUE(add.error().contains("size")) << add.error();

		const auto reduce =
			decode(encode(command::reduce(1, side_t::ask, 10, bogus)));
		ASSERT_FALSE(reduce.has_value())
			<< "decoded a REDUCE of size " << bogus;
		EXPECT_TRUE(reduce.error().contains("size")) << reduce.error();
	}
}

TEST(JournalRecord, AnUnknownOrderTypeIsRefused) {
	journal_record record = encode(populated_place());
	record.bytes[18]      = std::byte{0xFE};

	const auto restored = decode(record);
	ASSERT_FALSE(restored.has_value()) << "decoded an order type that does not exist";
	EXPECT_TRUE(restored.error().contains("order type")) << restored.error();
}

TEST(JournalRecord, AnUnknownTimeInForceIsRefused) {
	journal_record record = encode(populated_place());
	record.bytes[19]      = std::byte{0xFE};

	const auto restored = decode(record);
	ASSERT_FALSE(restored.has_value()) << "decoded a time in force that does not exist";
	EXPECT_TRUE(restored.error().contains("time in force")) << restored.error();
}

// A PLACE carries the routing symbol twice - once for the dispatcher and once
// inside the order - and command::place derives the first from the second, so
// they cannot disagree in a command that was actually built. A record where they
// do is therefore not one, and accepting it would route an order to a book for a
// different listing: the same fault `misrouted` reports, arriving silently.
TEST(JournalRecord, APlaceWhoseTwoSymbolsDisagreeIsRefused) {
	journal_record record = encode(populated_place());
	record.bytes[1]       = std::byte{0x99}; // the routing symbol only

	const auto restored = decode(record);
	ASSERT_FALSE(restored.has_value()) << "decoded a mis-routed PLACE";
	EXPECT_TRUE(restored.error().contains("routes to symbol"))
		<< restored.error();
}

// The saving that makes a defined layout free rather than a tax: the compiler's
// representation of a command is 48 bytes, eight of them padding around a union.
TEST(JournalRecord, TheEncodedFormIsSmallerThanTheObjectRepresentation) {
	static_assert(sizeof(journal_record) == 40);
	static_assert(sizeof(command) == 48);
	EXPECT_LT(sizeof(journal_record), sizeof(command));
}

} // namespace
