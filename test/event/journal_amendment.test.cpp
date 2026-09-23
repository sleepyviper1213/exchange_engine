#include "event/command.hpp"
#include "event/journal_record.hpp"
#include "orders/amendment.hpp"
#include "orders/side.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>

// MODIFY's share of the journal format, in its own suite because what is
// interesting about it is what it does *not* do: an amendment is an order id, a
// price, a quantity and a receipt time, and every one of those already had an
// offset. So the command cost the format nothing - no new bytes, no stride
// change, and every journal written before it existed still reads back as the
// same stream of commands.
//
// JournalRecord covers the other four tags and the layout as a whole.

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::event;
using namespace exchange::engine::orders;

namespace {

/// @brief A MODIFY with every field non-zero and distinct, so a round trip that
///        crossed two of them over fails rather than coincidentally passing.
[[nodiscard]] command populated_amendment() {
	return command::modify(0x1122'3344U,
						   amendment{.id        = 0x0102'0304'0506'0708ULL,
									 .price     = 0x5566'7788U,
									 .quantity  = 0x0D0E'0F10,
									 .timestamp = 0xF0E0'D0C0'B0A0'9080ULL});
}

/// @brief The byte at @p offset of @p record.
[[nodiscard]] std::uint8_t amend_byte(const journal_record &record,
									  std::size_t offset) {
	return std::to_integer<std::uint8_t>(record.bytes[offset]);
}

/// @brief The little-endian value of @p width bytes starting at @p offset.
[[nodiscard]] std::uint64_t amend_field(const journal_record &record,
										std::size_t offset, std::size_t width) {
	std::uint64_t value = 0;
	for (std::size_t i = width; i-- > 0;)
		value = (value << 8U) | amend_byte(record, offset + i);
	return value;
}

} // namespace

TEST(JournalAmendment, AModifyRoundTripsWithEveryFieldIntact) {
	const command original = populated_amendment();
	const auto restored    = decode(encode(original));

	ASSERT_TRUE(restored.has_value()) << restored.error();
	EXPECT_EQ(restored->type, command_type::MODIFY);
	EXPECT_EQ(restored->symbol, 0x1122'3344U);
	EXPECT_EQ(restored->as_modify(), original.as_modify());
}

TEST(JournalAmendment, ExtremeValuesSurvive) {
	const command widest = command::modify(
		std::numeric_limits<symbol_id_t>::max(),
		amendment{.id        = std::numeric_limits<order_id_t>::max(),
				  .price     = std::numeric_limits<price_t>::max(),
				  .quantity  = std::numeric_limits<quantity_t>::max(),
				  .timestamp = std::numeric_limits<timestamp_t>::max()});

	const auto restored = decode(encode(widest));
	ASSERT_TRUE(restored.has_value()) << restored.error();
	EXPECT_EQ(restored->as_modify(), widest.as_modify());
}

// The claim the command was added on: it borrows PLACE's offsets rather than
// taking new ones. If somebody gives MODIFY a layout of its own, this is what
// says so - and the stride below is what says every journal on disk still
// parses.
TEST(JournalAmendment, ItBorrowsThePlaceArmsOffsets) {
	static_assert(journal_record::SIZE == 40,
				  "MODIFY must not have grown the record");
	const journal_record record = encode(populated_amendment());

	EXPECT_EQ(amend_byte(record, 0),
			  static_cast<std::uint8_t>(command_type::MODIFY));
	EXPECT_EQ(amend_field(record, 1, 4), 0x1122'3344ULL);           // symbol
	EXPECT_EQ(amend_field(record, 5, 8), 0x0102'0304'0506'0708ULL); // id
	EXPECT_EQ(amend_field(record, 20, 4), 0x5566'7788ULL);          // price
	EXPECT_EQ(amend_field(record, 28, 4), 0x0D0E'0F10ULL);          // quantity
	EXPECT_EQ(amend_field(record, 32, 8),
			  0xF0E0'D0C0'B0A0'9080ULL);                            // timestamp
}

// The bytes an arm does not use are zero, so encoding the same command twice
// gives the same record and two journals of one flow compare equal.
TEST(JournalAmendment, UnusedBytesAreZeroed) {
	const command change =
		command::modify(9, amendment{.id = 42, .price = 100, .quantity = 5});
	EXPECT_EQ(encode(change), encode(change));

	const journal_record record = encode(change);
	// A MODIFY leaves the order's own symbol_id, the side, the type, the
	// time-in-force and the stop price to PLACE - bytes 13 through 19, and 24
	// through 27.
	for (std::size_t offset = 13; offset < 20; ++offset)
		EXPECT_EQ(amend_byte(record, offset), 0U) << "byte " << offset;
	for (std::size_t offset = 24; offset < 28; ++offset)
		EXPECT_EQ(amend_byte(record, offset), 0U) << "byte " << offset;
}

// Id zero is the anonymous sentinel, which is never indexed and so never
// amendable. The book drops such a request in silence for want of anyone to
// tell; here there *is* somewhere to put the complaint, so a record this engine
// never wrote is refused rather than replayed into a no-op.
TEST(JournalAmendment, AnAmendmentForTheAnonymousIdIsRefused) {
	journal_record record{};
	record.bytes[0] = static_cast<std::byte>(command_type::MODIFY);
	// Everything else stays zero, including the id.
	EXPECT_FALSE(decode(record).has_value());
}

// Deliberately *not* refused. A non-positive quantity reaches modify_order,
// which has a client to answer and answers it with NON_POSITIVE_QUANTITY - the
// same division of labour that lets a PLACE carrying a bad quantity decode and
// be rejected by the book. ADD and REDUCE are the pair that must be caught at
// decode, because they reach helpers with nobody to report to.
TEST(JournalAmendment, ANonPositiveQuantityDecodesAndIsTheBooksToRefuse) {
	const command change =
		command::modify(9, amendment{.id = 42, .price = 100, .quantity = -5});

	const auto restored = decode(encode(change));
	ASSERT_TRUE(restored.has_value()) << restored.error();
	EXPECT_EQ(restored->as_modify().quantity, -5);
}

// A MODIFY and a CANCEL share the id offset and differ only in the tag, which
// makes the tag exactly the thing worth checking - a replay that confused them
// would withdraw orders it was asked to resize.
TEST(JournalAmendment, ItStaysDistinctFromACancelOfTheSameOrder) {
	const command change =
		command::modify(7, amendment{.id = 42, .price = 0, .quantity = 0});
	EXPECT_NE(encode(change), encode(command::cancel(7, 42)))
		<< "the tag did not reach the bytes";
}
