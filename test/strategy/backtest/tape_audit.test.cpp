#include "strategy/backtest/tape_audit.hpp"

#include "backtest.fixture.hpp"
#include "strategy/backtest/modelled_fill.hpp"
#include "strategy/backtest/tape_audit_report.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <span>

// The audit exists to bound the fill model from above, so every case here is
// about it erring on the strict side. An auditor that is too generous is worse
// than no auditor: it launders the model's optimism into a number that looks
// like evidence.
//
// The two that matter are consumption - one print cannot justify two fills -
// and the window, which has to be wide enough to admit a real fill from a 100ms
// frame and no wider.

using namespace exchange;
using namespace exchange::strategy::backtest;

namespace {

/// @brief A print on the tape. @p aggressor is the side that crossed.
market_data::trade_print tape_print(std::uint64_t at_ns, std::int64_t price,
									std::int64_t qty, side_t aggressor) {
	market_data::trade_print out;
	out.event_time = std::chrono::nanoseconds{static_cast<std::int64_t>(at_ns)};
	out.price      = price;
	out.qty        = qty;
	out.aggressor  = aggressor;
	return out;
}

/// @brief A claim that we were filled resting on @p our_side.
modelled_fill claim(std::uint64_t at_ns, price_t price, side_t our_side,
					volume_t volume) {
	return modelled_fill{.at_ns    = at_ns,
						 .price    = price,
						 .our_side = our_side,
						 .volume   = volume};
}

} // namespace

TEST(TapeAudit, BacksAClaimTheTapePrintedAtTheSamePriceAndSide) {
	const engine::symbol_spec spec = unit_listing();
	tape_audit tape;
	// We rested on the bid at 100, so the aggressor was a seller.
	tape.on_trade(tape_print(1000, 100, 5, side_t::ask));

	const auto fills = std::to_array({claim(1000, 100, side_t::bid, 5)});
	const tape_audit_report out = tape.audit(fills, spec);

	EXPECT_EQ(out.fills, 1U);
	EXPECT_EQ(out.fills_supported, 1U);
	EXPECT_EQ(out.lots_supported, 5);
	EXPECT_EQ(out.lots_unsupported, 0);
	EXPECT_TRUE(is_fully_supported(out));
	EXPECT_EQ(supported_bps(out), 10000);
}

TEST(TapeAudit, RefusesAClaimWithNoPrintBehindIt) {
	const engine::symbol_spec spec = unit_listing();
	tape_audit tape;
	// The tape is recording and covers the window - it simply shows nothing
	// that could have filled us. That is the model claiming volume the venue
	// never traded, which is the finding; a *silent* tape would be the
	// different and weaker statement that we cannot tell.
	tape.on_trade(tape_print(1000, 105, 5, side_t::bid));

	const auto fills = std::to_array({claim(1000, 100, side_t::bid, 5)});
	const tape_audit_report out = tape.audit(fills, spec);

	EXPECT_EQ(out.fills_uncovered, 0U);
	EXPECT_EQ(out.fills_unsupported, 1U);
	EXPECT_EQ(out.lots_unsupported, 5);
	EXPECT_EQ(supported_bps(out), 0);
	EXPECT_FALSE(is_fully_supported(out));
}

TEST(TapeAudit, RefusesAPrintOnTheWrongSide) {
	const engine::symbol_spec spec = unit_listing();
	tape_audit tape;
	// A buyer crossed. That fills somebody's *ask*, not our bid.
	tape.on_trade(tape_print(1000, 100, 5, side_t::bid));

	const auto fills = std::to_array({claim(1000, 100, side_t::bid, 5)});
	const tape_audit_report out = tape.audit(fills, spec);

	EXPECT_EQ(out.lots_unsupported, 5)
		<< "a trade on our own side cannot have filled us";
}

TEST(TapeAudit, RefusesAPrintAtADifferentPrice) {
	const engine::symbol_spec spec = unit_listing();
	tape_audit tape;
	tape.on_trade(tape_print(1000, 101, 5, side_t::ask));

	const auto fills = std::to_array({claim(1000, 100, side_t::bid, 5)});
	const tape_audit_report out = tape.audit(fills, spec);

	EXPECT_EQ(out.lots_unsupported, 5);
}

TEST(TapeAudit, OnePrintCannotJustifyTwoFills) {
	const engine::symbol_spec spec = unit_listing();
	tape_audit tape;
	// Five lots traded, once.
	tape.on_trade(tape_print(1000, 100, 5, side_t::ask));

	// The model claims two fills of five, both inside the window.
	const auto fills = std::to_array(
		{claim(1000, 100, side_t::bid, 5), claim(1050, 100, side_t::bid, 5)});
	const tape_audit_report out = tape.audit(fills, spec);

	// Without consumption both would be called supported and the audit would
	// report a model that invented five lots as perfectly accurate.
	EXPECT_EQ(out.fills_supported, 1U);
	EXPECT_EQ(out.fills_unsupported, 1U);
	EXPECT_EQ(out.lots_supported, 5);
	EXPECT_EQ(out.lots_unsupported, 5);
	EXPECT_EQ(supported_bps(out), 5000);
}

TEST(TapeAudit, ReportsAPartialWhenThePrintIsSmallerThanTheClaim) {
	const engine::symbol_spec spec = unit_listing();
	tape_audit tape;
	tape.on_trade(tape_print(1000, 100, 2, side_t::ask));

	const auto fills = std::to_array({claim(1000, 100, side_t::bid, 5)});
	const tape_audit_report out = tape.audit(fills, spec);

	EXPECT_EQ(out.fills_partial, 1U);
	EXPECT_EQ(out.lots_supported, 2);
	EXPECT_EQ(out.lots_unsupported, 3);
}

TEST(TapeAudit, AcceptsAPrintOneFrameBeforeTheInferredFill) {
	const engine::symbol_spec spec = unit_listing();
	tape_audit tape;
	// The trade happened 80ms before the frame the model noticed it in, which
	// is the normal case on a 100ms feed rather than an anomaly.
	tape.on_trade(tape_print(920'000'000, 100, 5, side_t::ask));

	const auto fills =
		std::to_array({claim(1'000'000'000, 100, side_t::bid, 5)});
	const tape_audit_report out = tape.audit(fills, spec);

	EXPECT_EQ(out.lots_supported, 5);
}

TEST(TapeAudit, RefusesAPrintOutsideTheWindow) {
	const engine::symbol_spec spec = unit_listing();
	tape_audit tape;
	// Half a second earlier - five frames back. Whatever filled us, not this.
	tape.on_trade(tape_print(500'000'000, 100, 5, side_t::ask));
	// And a later, irrelevant print, so the tape demonstrably *spans* the fill.
	// Without it the fill sits past the end of the recording and is set aside
	// as uncovered, which is a different claim than the one being tested.
	tape.on_trade(tape_print(2'000'000'000, 105, 5, side_t::bid));

	const auto fills =
		std::to_array({claim(1'000'000'000, 100, side_t::bid, 5)});
	const tape_audit_report out = tape.audit(fills, spec);

	EXPECT_EQ(out.fills_uncovered, 0U) << "the tape spans this fill";
	EXPECT_EQ(out.lots_unsupported, 5);
}

TEST(TapeAudit, TheWindowIsAParameterOfTheRecordingsCadence) {
	const engine::symbol_spec spec = unit_listing();
	tape_audit tape;
	tape.on_trade(tape_print(500'000'000, 100, 5, side_t::ask));

	const auto fills =
		std::to_array({claim(1'000'000'000, 100, side_t::bid, 5)});
	// A 1000ms capture wants a 1000ms window, and then the same print counts.
	const tape_audit_report out = tape.audit(fills, spec, 1'000'000'000);

	EXPECT_EQ(out.lots_supported, 5);
	EXPECT_EQ(out.tolerance_ns, 1'000'000'000U);
}

TEST(TapeAudit, AuditingTwiceGivesTheSameAnswer) {
	const engine::symbol_spec spec = unit_listing();
	tape_audit tape;
	tape.on_trade(tape_print(1000, 100, 5, side_t::ask));

	const auto fills = std::to_array({claim(1000, 100, side_t::bid, 5)});
	const tape_audit_report first  = tape.audit(fills, spec);
	const tape_audit_report second = tape.audit(fills, spec);

	// The consumption is scratch state, not a property of the tape. If it
	// leaked, the second pass would find every print already spent and report
	// the model as fabricating everything.
	EXPECT_EQ(first.lots_supported, second.lots_supported);
	EXPECT_EQ(second.lots_supported, 5);
}

TEST(TapeAudit, AnEmptyRunIsNotPerfectlyAccurate) {
	const engine::symbol_spec spec = unit_listing();
	tape_audit tape;
	tape.on_trade(tape_print(1000, 100, 5, side_t::ask));

	const tape_audit_report out = tape.audit({}, spec);

	EXPECT_EQ(out.fills, 0U);
	EXPECT_EQ(supported_bps(out), 0) << "nothing claimed is not 100% supported";
	EXPECT_FALSE(is_fully_supported(out));
	EXPECT_EQ(out.prints_seen, 1U);
}

TEST(TapeAudit, DoesNotBlameTheModelForFillsTheTapeNeverCovered) {
	const engine::symbol_spec spec = unit_listing();
	tape_audit tape;
	// The tape only starts at t = 10s. Anything before that is outside what
	// this recording can speak for.
	tape.on_trade(tape_print(10'000'000'000, 100, 5, side_t::ask));

	const auto fills            = std::to_array({
		claim(1'000'000'000, 100, side_t::bid, 5),  // before the tape begins
		claim(10'000'000'000, 100, side_t::bid, 5), // inside it, and supported
	});
	const tape_audit_report out = tape.audit(fills, spec);

	EXPECT_EQ(out.fills, 2U);
	EXPECT_EQ(out.fills_uncovered, 1U);
	EXPECT_EQ(out.lots_uncovered, 5);
	// The early one is set aside rather than counted against the model: it says
	// the two captures were started apart, not that the model invented volume.
	EXPECT_EQ(out.fills_unsupported, 0U);
	EXPECT_EQ(out.lots_supported, 5);
	EXPECT_EQ(lots_judged(out), 5);
	EXPECT_EQ(supported_bps(out), 10000) << "judged over what the tape covers";
	EXPECT_TRUE(is_fully_supported(out));
}

TEST(TapeAudit, EveryFillOutsideTheTapeIsNotFullSupport) {
	const engine::symbol_spec spec = unit_listing();
	tape_audit tape;
	tape.on_trade(tape_print(10'000'000'000, 100, 5, side_t::ask));

	const auto fills =
		std::to_array({claim(1'000'000'000, 100, side_t::bid, 5)});
	const tape_audit_report out = tape.audit(fills, spec);

	// Nothing was judged, so nothing was proven. A run whose fills all fall
	// outside the tape must not read as a clean bill of health.
	EXPECT_EQ(out.fills_uncovered, 1U);
	EXPECT_EQ(lots_judged(out), 0);
	EXPECT_EQ(supported_bps(out), 0);
	EXPECT_FALSE(is_fully_supported(out));
}

TEST(TapeAudit, ASilentTapeCoversNothingRatherThanRefusingEverything) {
	const engine::symbol_spec spec = unit_listing();
	tape_audit tape; // no prints at all

	const auto fills = std::to_array({claim(1000, 100, side_t::bid, 5)});
	const tape_audit_report out = tape.audit(fills, spec);

	// An empty tape is the degenerate case of the same rule: it covers no
	// window, so it convicts nobody.
	EXPECT_EQ(out.fills_uncovered, 1U);
	EXPECT_EQ(out.fills_unsupported, 0U);
	EXPECT_FALSE(is_fully_supported(out));
}
