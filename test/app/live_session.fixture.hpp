#pragma once
// A live session and the consumer thread's work, done by hand.
//
// Single-threaded on purpose, and it is not a shortcut. `live_session`'s
// contract is that everything is the producer thread's except
// `drain_and_publish`, which is the consumer's - and one thread calling both in
// sequence honours that exactly: there is still one writer at a time, and every
// hand-off still goes through the SPSC queue and the event channel it goes
// through in production. What a test that spawned a real consumer would gain is
// a timing-dependent assertion; what it would lose is the ability to say which
// frame produced which fill.
//
// So what these suites pin is the *wiring* - that depth reaches the book, that
// the gate sees every command, that fills reach the gate, the quoter and the
// post-trade monitor, that a gap withdraws what it seeded. Whether the two
// threads hand off correctly under contention is the SPSC queue's own suite and
// the channel's, which is where it belongs.

#include "session/live_session.hpp"

// unit_listing, seed, diff, level - the same feed builders the backtest suites
// drive their harness with. Reaching for them beats copying them: a second
// `level` or `seed` at global scope in one binary is an ODR violation the
// linker resolves by picking one. @see testing.md
#include "../strategy/backtest/backtest.fixture.hpp" // IWYU pragma: export

// A clock a test moves rather than waits for. The risk fixture already owns it
// and `live_session` is a template over exactly the concept it models.
#include "../risk_management/risk.fixture.hpp" // IWYU pragma: export

#include <cstddef>
#include <cstdint>

using exchange::session::live_session_options;
using exchange::session::live_session_report;

/// @brief The session under test: one listing, one hand-driven clock.
using test_live_session = exchange::session::live_session<manual_clock>;

/// @brief Ticks either side that make a two-sided market wide enough for a
///        passive quoter to improve on both sides of it. A 2-tick spread is
///        not: both sides improving by one lands on the same price, which the
///        quoter counts as `no_room` rather than crossing itself.
inline constexpr price_t LIVE_TOUCH_BID = 100;
inline constexpr price_t LIVE_TOUCH_ASK = 104;

/// @brief An ask that leaves the passive quoter no room: both sides improving
///        by one tick off a 2-tick spread land on the same price, which it
///        counts as @c no_room and declines rather than crossing itself. The
///        depth cases want that - with nothing of ours resting in the book, its
///        touch is the venue's touch and an assertion can say so.
inline constexpr price_t LIVE_TIGHT_ASK = 102;

/**
 * @brief A live session, plus the loop the consumer thread would be running.
 *
 * @note The listing is @c unit_listing, whose tick and lot are both 1 at scale
 *       0, so the feed's scaled numbers and the engine's ticks coincide and a
 *       case can write prices as plain integers while still going through the
 *       real @c symbol_spec conversions.
 */
class live_desk {
public:
	/// @brief Rounds of engine work one frame may take. A frame's commands can
	///        produce fills that the router hands back, and nothing here quotes
	///        again until the next frame, so two rounds is already generous -
	///        the bound exists so a wiring bug cannot hang the suite.
	static constexpr int MAX_ROUNDS = 8;

	explicit live_desk(live_session_options options = {})
		: run_(spec_, options, clock_) {}

	/// @brief Seed or repair the replica, then let the engine catch up.
	/// @return Whether the replica is live afterwards.
	bool seed_book(exchange::market_data::book_snapshot snapshot) {
		const bool live = run_.on_snapshot(std::move(snapshot));
		settle();
		return live;
	}

	/// @brief A two-sided market at @p bid / @p ask, as a fresh snapshot.
	bool seed_touch(std::int64_t bid = LIVE_TOUCH_BID,
					std::int64_t ask = LIVE_TOUCH_ASK, std::int64_t lots = 5,
					exchange::market_data::sequence_t sequence = 1) {
		return seed_book(
			seed(sequence, {level(bid, lots)}, {level(ask, lots)}));
	}

	/// @brief One diff, then the engine's work for it.
	exchange::market_data::sequence_action
	frame(exchange::market_data::depth_event event) {
		const auto action = run_.on_event(std::move(event));
		settle();
		return action;
	}

	/// @brief A diff that moves the touch to @p bid / @p ask.
	exchange::market_data::sequence_action
	move_touch(exchange::market_data::sequence_t sequence, std::int64_t bid,
			   std::int64_t ask, std::int64_t lots = 5,
			   std::uint64_t stamp_ns = 0) {
		return frame(
			diff(sequence, stamp_ns, {level(bid, lots)}, {level(ask, lots)}));
	}

	/// @brief Tell the session its stream was rebuilt, then settle.
	void reconnect() {
		run_.invalidate();
		settle();
	}

	/// @brief Move the local clock and let the watchdogs look at it.
	void advance(std::uint64_t delta_ns) {
		clock_.advance(delta_ns);
		run_.pump();
	}

	[[nodiscard]] test_live_session &session() noexcept { return run_; }

	[[nodiscard]] const live_session_report &report() const noexcept {
		return run_.report();
	}

	/// @brief The engine's book for this listing.
	[[nodiscard]] const exchange::engine::order_book &book() const {
		return *run_.partition().book(spec_.id());
	}

	/// @brief Net position in lots, signed.
	[[nodiscard]] volume_t net() const noexcept {
		return run_.positions().net_lots(spec_.id());
	}

	/// @brief Executions the post-trade monitor has counted - the honest
	///        answer to "did anything actually trade", because it is counted at
	///        the far end of the whole loop.
	[[nodiscard]] std::uint64_t fills() const noexcept {
		return run_.monitor().fills().total_executions();
	}

	[[nodiscard]] const exchange::engine::symbol_spec &spec() const noexcept {
		return spec_;
	}

private:
	/// @brief What the consumer thread does, interleaved with the producer's
	///        pump so neither the command queue nor the event channel can fill.
	void settle() {
		for (int round = 0; round < MAX_ROUNDS; ++round) {
			const std::size_t applied = run_.drain_and_publish();
			run_.pump_all();
			if (applied == 0) return;
		}
	}

	exchange::engine::symbol_spec spec_ = unit_listing();
	manual_clock clock_;
	test_live_session run_;
};
