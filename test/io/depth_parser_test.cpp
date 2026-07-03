#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>

#include "io/binance_depth.hpp"

using binance::DepthParser;
using binance::parse_binance_depth;
using binance::parse_binance_depth_update;

// DepthParser is the reusable-buffer variant of the free parse_* functions. These
// tests pin down two things the free functions can't exercise: that a single
// instance stays correct when reused across many frames (buffer amortization must
// not leak state between calls), and that it recovers cleanly after a bad frame.

namespace {
constexpr std::string_view kSnapshot =
    R"({"lastUpdateId":123,)"
    R"("bids":[["153.45","10.00"],["153.44","5.50"]],)"
    R"("asks":[["153.46","8.00"],["153.47","2.00"]]})";

constexpr std::string_view kUpdate =
    R"({"e":"depthUpdate","E":1571889248277,"s":"SOLUSDT",)"
    R"("U":390497796,"u":390497878,)"
    R"("b":[["153.45","0.00"],["153.44","5.50"]],)"
    R"("a":[["153.46","8.00"]]})";
} // namespace

// --------------------------------------------------------------------------
// Equivalence with the one-shot free functions
// --------------------------------------------------------------------------

TEST(DepthParserTest, UpdateMatchesFreeFunction) {
    DepthParser parser;
    const auto reused = parser.parse_update(kUpdate, 2, 2);
    const auto oneshot = parse_binance_depth_update(kUpdate, 2, 2);
    ASSERT_TRUE(reused.has_value()) << reused.error();
    ASSERT_TRUE(oneshot.has_value()) << oneshot.error();

    EXPECT_EQ(reused->eventTime, oneshot->eventTime);
    EXPECT_EQ(reused->firstUpdateId, oneshot->firstUpdateId);
    EXPECT_EQ(reused->finalUpdateId, oneshot->finalUpdateId);
    ASSERT_EQ(reused->bids.size(), oneshot->bids.size());
    ASSERT_EQ(reused->asks.size(), oneshot->asks.size());
    for (std::size_t i = 0; i < reused->bids.size(); ++i) {
        EXPECT_EQ(reused->bids[i].price, oneshot->bids[i].price);
        EXPECT_EQ(reused->bids[i].volume, oneshot->bids[i].volume);
    }
}

TEST(DepthParserTest, SnapshotMatchesFreeFunction) {
    DepthParser parser;
    const auto reused = parser.parse_snapshot(kSnapshot, 2, 2);
    const auto oneshot = parse_binance_depth(kSnapshot, 2, 2);
    ASSERT_TRUE(reused.has_value()) << reused.error();
    ASSERT_TRUE(oneshot.has_value()) << oneshot.error();

    EXPECT_EQ(reused->lastUpdateId, oneshot->lastUpdateId);
    ASSERT_EQ(reused->bids.size(), oneshot->bids.size());
    EXPECT_EQ(reused->bids[0].price, oneshot->bids[0].price);
    EXPECT_EQ(reused->asks[0].price, oneshot->asks[0].price);
}

// --------------------------------------------------------------------------
// Reuse across frames — the whole point of the class
// --------------------------------------------------------------------------

TEST(DepthParserTest, ReusedAcrossManyFramesStaysCorrect) {
    DepthParser parser;
    for (int i = 0; i < 100; ++i) {
        const auto up = parser.parse_update(kUpdate, 2, 2);
        ASSERT_TRUE(up.has_value()) << "frame " << i << ": " << up.error();
        ASSERT_EQ(up->bids.size(), 2u);
        EXPECT_EQ(up->bids[1].price, 15344u);
        EXPECT_EQ(up->bids[1].volume, 550);
        ASSERT_EQ(up->asks.size(), 1u);
        EXPECT_EQ(up->asks[0].volume, 800);
    }
}

TEST(DepthParserTest, ShortFrameAfterLongFrameHasNoStaleData) {
    // The reused input buffer keeps its capacity from the longer frame; a shorter
    // frame that follows must parse only its own bytes, not leftover tail bytes.
    const std::string long_frame =
        std::string(R"({"E":1,"U":1,"u":9,"b":[)") +
        R"(["153.45","1.00"],["153.44","2.00"],["153.43","3.00"]],)" +
        R"("a":[["153.46","4.00"]]})";
    const std::string short_frame =
        R"({"E":2,"U":10,"u":10,"b":[["153.40","7.00"]],"a":[]})";

    DepthParser parser;
    ASSERT_TRUE(parser.parse_update(long_frame, 2, 2).has_value());

    const auto up = parser.parse_update(short_frame, 2, 2);
    ASSERT_TRUE(up.has_value()) << up.error();
    EXPECT_EQ(up->finalUpdateId, 10u);
    ASSERT_EQ(up->bids.size(), 1u);
    EXPECT_EQ(up->bids[0].price, 15340u);
    EXPECT_EQ(up->bids[0].volume, 700);
    EXPECT_TRUE(up->asks.empty());
}

TEST(DepthParserTest, GrowsBufferForLargerFollowingFrame) {
    const std::string small = R"({"E":1,"U":1,"u":1,"b":[["1.00","1.00"]],"a":[]})";
    std::string big = R"({"E":2,"U":2,"u":2,"b":[)";
    for (int i = 0; i < 50; ++i) {
        if (i != 0) big += ',';
        big += R"(["1.00","1.00"])";
    }
    big += R"(],"a":[]})";

    DepthParser parser;
    ASSERT_TRUE(parser.parse_update(small, 2, 2).has_value());
    const auto up = parser.parse_update(big, 2, 2);
    ASSERT_TRUE(up.has_value()) << up.error();
    EXPECT_EQ(up->bids.size(), 50u);
}

// --------------------------------------------------------------------------
// Error handling — a bad frame must not poison the parser
// --------------------------------------------------------------------------

TEST(DepthParserTest, RecoversAfterMalformedFrame) {
    DepthParser parser;
    EXPECT_FALSE(parser.parse_update("{not json", 2, 2).has_value());

    // The very next valid frame must parse correctly.
    const auto up = parser.parse_update(kUpdate, 2, 2);
    ASSERT_TRUE(up.has_value()) << up.error();
    EXPECT_EQ(up->finalUpdateId, 390497878ull);
}

TEST(DepthParserTest, RejectsNonNumericQty) {
    DepthParser parser;
    const auto up = parser.parse_update(
        R"({"E":1,"U":1,"u":1,"b":[["153.45","oops"]],"a":[]})", 2, 2);
    EXPECT_FALSE(up.has_value());
}

// --------------------------------------------------------------------------
// Move semantics — the class owns a unique_ptr Impl
// --------------------------------------------------------------------------

TEST(DepthParserTest, MoveConstructedParserIsUsable) {
    DepthParser src;
    ASSERT_TRUE(src.parse_update(kUpdate, 2, 2).has_value());

    DepthParser moved(std::move(src));
    const auto up = moved.parse_update(kUpdate, 2, 2);
    ASSERT_TRUE(up.has_value()) << up.error();
    EXPECT_EQ(up->bids.size(), 2u);
}
