#include "event/format.hpp"

#include "event/command.hpp"
#include "orders/order.hpp"
#include "orders/types.hpp"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <string>

// How a command renders.
//
// Untested until now, which is how a rewrite of the tag rendering went in
// unverified: the tag used to be printed by a hand-written switch in
// event/format.hpp and is now carried by `command_type`'s generated `format_as`.
// The two agree, and this suite is what says so - and what will notice if a
// command type is ever added to the enum without a list entry to name it.

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::event;

namespace {

using orders::order;

/// @brief A PLACE for one lot at one price, with nothing incidental set.
[[nodiscard]] command placed(order_id_t id, price_t price, quantity_t qty) {
	return command::place(order{
		.id        = id,
		.symbol_id = 7,
		.side      = side_t::bid,
		.price     = price,
		.qty       = qty,
	});
}

} // namespace

TEST(CommandFormat, EachTagPrintsItsOwnName) {
	// The four names come from COMMAND_TYPE_LIST, so this is also the check that
	// the list and the enum have not drifted apart.
	EXPECT_TRUE(fmt::format("{}", placed(1, 100, 5)).contains("cmd[PLACE "));
	EXPECT_TRUE(
		fmt::format("{}", command::cancel(7, 1)).contains("cmd[CANCEL "));
	EXPECT_TRUE(fmt::format("{}", command::add(7, side_t::bid, 100, 5))
					.contains("cmd[ADD "));
	EXPECT_TRUE(fmt::format("{}", command::reduce(7, side_t::ask, 100, 5))
					.contains("cmd[REDUCE "));
}

TEST(CommandFormat, TheListingIsAlwaysNamed) {
	// The one field no payload carries and the whole wrapper exists for.
	EXPECT_TRUE(fmt::format("{}", command::cancel(7, 1)).contains("sym=7"));
	EXPECT_TRUE(fmt::format("{}", placed(1, 100, 5)).contains("sym=7"));
}

TEST(CommandFormat, APlacePrintsItsWholeOrder) {
	const std::string text = fmt::format("{}", placed(42, 100, 5));

	EXPECT_TRUE(text.contains("Order["))
		<< "delegated to orders/format.hpp rather than restated here: " << text;
	EXPECT_TRUE(text.contains("id=42")) << text;
}

TEST(CommandFormat, ACancelPrintsOnlyTheIdItCarries) {
	const std::string text = fmt::format("{}", command::cancel(7, 42));

	EXPECT_TRUE(text.contains("id=42")) << text;
	EXPECT_FALSE(text.contains("order["))
		<< "the union's other arms are not live and must not be read: " << text;
}

TEST(CommandFormat, ALevelChangePrintsSidePriceAndSize) {
	const std::string text =
		fmt::format("{}", command::add(7, side_t::ask, 100, 5));

	EXPECT_TRUE(text.contains("@100")) << text;
	EXPECT_TRUE(text.contains('5')) << text;
	EXPECT_FALSE(text.contains("id="))
		<< "an ADD is anonymous - there is no id arm to print: " << text;
}

TEST(CommandFormat, TheRecordIsClosed) {
	// A truncated record in a log is worse than no record: it reads as the next
	// field's prefix.
	EXPECT_TRUE(fmt::format("{}", placed(1, 100, 5)).ends_with("]"));
	EXPECT_TRUE(fmt::format("{}", command::cancel(7, 1)).ends_with("]"));
}

TEST(CommandFormat, WidthAppliesToTheWholeRecord) {
	// What deriving from nested_formatter buys, and the reason every formatter in
	// this module does: `{:>40}` aligns the record in a log column rather than
	// padding whatever field happens to be last.
	const std::string padded = fmt::format("{:>40}", command::cancel(7, 1));
	const std::string bare   = fmt::format("{}", command::cancel(7, 1));

	ASSERT_LT(bare.size(), 40U) << "the fixture no longer tests padding";
	EXPECT_EQ(padded.size(), 40U);
	EXPECT_TRUE(padded.ends_with(bare)) << "right-aligned, not interleaved";
}

TEST(CommandFormat, TheTagAloneRendersAsItsName) {
	// command_type is printable in its own right, which is what the formatter
	// above now leans on. Worth pinning separately: a caller logging just the tag
	// must not have to reach into the record's rendering to get it.
	EXPECT_EQ(fmt::format("{}", command_type::PLACE), "PLACE");
	EXPECT_EQ(fmt::format("{}", command_type::REDUCE), "REDUCE");
	EXPECT_EQ(to_string(command_type::CANCEL), "CANCEL");
}
