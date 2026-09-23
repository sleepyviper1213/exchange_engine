#pragma once

#include "event/command.hpp"
#include "market_data/l2_book.hpp"
#include "market_data/normalised.hpp"
#include "market_data/reconstructor.hpp"
#include "market_data/types.hpp"
#include "orders/types.hpp"
#include "strategy_export.hpp"
#include "symbol/symbol_spec.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace exchange::strategy::backtest {

/**
 * @brief Turns one listing's gap-checked depth feed into engine commands.
 *
 * Feed it normalised events and snapshots; it appends the @c event::command%s
 * that bring the engine's book to the same aggregate depth. The commands are
 * ordinary ADD / REDUCE addressed to @p symbol, so they route through
 * @c execution::dispatcher and execute on whichever partition owns the listing
 * like anything else.
 *
 * @par Why it diffs two books instead of translating the event
 * The obvious implementation - read each changed level out of the event and
 * emit the difference - is wrong, because @c depth_reconstructor moves the
 * replica in ways no single event describes. A snapshot reseeds the book and
 * then replays every buffered event onto it. A gap clears it outright. A level
 * falling out of the retained window disappears with no event naming it. Each
 * would leave the engine's book holding depth the venue no longer publishes,
 * and the divergence is silent and permanent.
 *
 * So the rule here is a single one that covers every case: hold a @c mirror_ of
 * what the engine has already been told, and after any operation emit whatever
 * turns the mirror into the replica. The invariant is checkable - once the
 * commands drain, the engine's aggregate depth equals @c replica() - and it
 * holds through gaps, resyncs and evictions without any of them being special
 * cases.
 *
 * The cost is a walk of both books per call rather than of the changed levels
 * alone. Both are contiguous, price-sorted, best-first, so it is a linear merge
 * over two arrays that share a comparator - for the depth an L2 window retains,
 * cheaper than the branchier alternative and considerably easier to be sure of.
 *
 * @warning A gap emits REDUCE for the whole book, and it must. When the replica
 *          dies, liquidity seeded from it is no longer evidence about the
 *          venue, and matching against it would be matching against a snapshot
 *          of the past. Orders this process originated are untouched - they are
 *          identified, and only the anonymous depth is withdrawn.
 *
 * @note Not thread-safe, and single-producer by construction: one feed, one
 *       listing, one bridge, feeding one command stream.
 */
class depth_feed_bridge {
public:
	using command = engine::event::command;
	using level   = market_data::l2_book::price_level;

	/**
	 * @brief Bridge the feed for one listing.
	 *
	 * @param spec The listing's trading conventions. It supplies both the
	 *        engine-side id every command is addressed to and - the reason it
	 * is needed rather than just the id - the tick and lot grid that turns the
	 * feed's scaled decimals into the engine's ticks and lots. Must outlive the
	 * bridge; reference data is owned by the registry and changes between
	 * sessions, not between frames.
	 * @param options Passed to the reconstructor that gap-checks the feed.
	 */
	STRATEGY_EXPORT explicit depth_feed_bridge(
		const engine::symbol_spec &spec,
		market_data::reconstructor_options options = {}) noexcept;

	/**
	 * @brief Feed one normalised diff; append the commands it implies.
	 *
	 * @param event The decoded event; consumed.
	 * @param out Commands are appended, never cleared, so a caller can batch a
	 *        whole drain into one buffer and submit it with @c submit_range.
	 * @return What the sequencer did with it. @c apply means the commands
	 *         describe an in-sequence update; @c gap means the replica died and
	 *         the appended commands withdraw the depth it had seeded.
	 *         @c buffer and @c discard normally append nothing.
	 */
	STRATEGY_EXPORT market_data::sequence_action
	on_event(market_data::depth_event event, std::vector<command> &out);

	/**
	 * @brief Seed or repair from a snapshot; append the commands it implies.
	 *
	 * @param snapshot The full depth; consumed.
	 * @param out Commands are appended, never cleared.
	 * @return Whether the replica is live afterwards. When it is not, a newer
	 *         snapshot is needed and nothing has been seeded.
	 */
	STRATEGY_EXPORT bool on_snapshot(const market_data::book_snapshot &snapshot,
									 std::vector<command> &out);

	/**
	 * @brief Declare the replica stale - a transport reconnect, a dropped
	 *        frame - and withdraw the depth it seeded.
	 */
	STRATEGY_EXPORT void invalidate(std::vector<command> &out);

	/**
	 * @brief Note that a match took @p lots of the seeded depth at @p price.
	 *
	 * @par Why the mirror has to be told
	 * @c mirror_ is not a copy of the replica for its own sake - it is this
	 * class's model of *what the engine's book currently holds in anonymous
	 * depth*, and every command emitted is a delta against it. That model is
	 * exact only for as long as nothing but this bridge moves that depth, and
	 * one thing does: an identified order crossing into it. The book consumes
	 * the seeded liquidity, the mirror does not notice, and the next diff is
	 * therefore computed from a level size the book no longer has - leaving it
	 * permanently short by whatever was taken, with no event that could ever
	 * repair it.
	 *
	 * Telling the bridge closes that hole with the mechanism already here: the
	 * mirror drops by what was taken, so the next frame restating the venue's
	 * size emits the ADD that puts it back. Note *when* it comes back - on the
	 * next frame, not immediately. That is deliberate. Restoring it in the same
	 * breath would leave the book crossed against whatever remainder of the
	 * aggressor rested, and a fill model looking at that cross would fill the
	 * remainder against liquidity it had already consumed.
	 *
	 * @par What it assumes
	 * That our trade did not move the market: the venue is still showing the
	 * size it was showing, and the next frame's restatement is the truth. For a
	 * participant small relative to the book that is the usual simplification
	 * and it is the one a replay can support - the recording cannot tell us
	 * what the venue *would* have published had we been in it. @see
	 * backtest::crossing_fill_model on the other half of the same assumption.
	 *
	 * @param side The side the consumed depth was resting on.
	 * @param price The level, in the listing's ticks.
	 * @param lots How much was taken, in the listing's lots.
	 *
	 * @note A level the mirror does not carry, or a size larger than it holds,
	 *       clamps to empty rather than going negative. Both mean this bridge
	 *       did not seed what was consumed - an order matching another
	 *       identified order, say - which is not this class's business.
	 */
	STRATEGY_EXPORT void consumed(side_t side, price_t price, volume_t lots);

	/// @brief The listing every emitted command is addressed to.
	[[nodiscard]] STRATEGY_EXPORT symbol_id_t symbol() const noexcept;

	/// @brief The venue replica. Meaningful only while @c is_alive().
	[[nodiscard]] STRATEGY_EXPORT const market_data::l2_book &
	replica() const noexcept;

	/**
	 * @brief The depth the engine's book has already been told about.
	 *
	 * Equal to @c replica() after every call that moves the feed - that is the
	 * invariant this class maintains. Exposed so a test, or an operator, can
	 * assert it rather than take it on trust.
	 *
	 * @note @c consumed is the one call that parts them, and it does so to keep
	 *       the *stated* meaning of this book true: after a match has taken
	 * some of the seeded liquidity, what the engine holds is no longer what the
	 *       venue publishes, and the mirror follows the engine. The next diff
	 *       restores both.
	 */
	[[nodiscard]] STRATEGY_EXPORT const market_data::l2_book &
	mirror() const noexcept;

	/// @brief Whether the feed is seeded and in sequence.
	[[nodiscard]] STRATEGY_EXPORT bool is_alive() const noexcept;

	/// @brief Whether the caller owes this bridge a snapshot fetch.
	[[nodiscard]] STRATEGY_EXPORT bool needs_snapshot() const noexcept;

	/// @brief Note that a snapshot fetch is in flight. @see
	///        depth_reconstructor::snapshot_requested
	STRATEGY_EXPORT void snapshot_requested() noexcept;

	/// @brief Note that the in-flight fetch failed.
	STRATEGY_EXPORT void snapshot_failed() noexcept;

	/// @brief The reconstructor, for its feed-health counters.
	[[nodiscard]] STRATEGY_EXPORT const market_data::depth_reconstructor &
	reconstructor() const noexcept;

	/// @brief Commands emitted since construction - how much book churn the
	///        feed has cost the engine.
	[[nodiscard]] STRATEGY_EXPORT std::uint64_t
	commands_emitted() const noexcept;

	/**
	 * @brief Level changes the listing's own spec could not express, and which
	 *        were therefore not passed to the engine.
	 *
	 * Should be zero, and a non-zero reading is a configuration fault rather
	 * than a market event - the same kind of number as
	 * @c engine_partition::misrouted. It counts a venue price that is not on
	 * this listing's tick grid, a size not on its lot grid, or either one past
	 * what the engine's 32-bit ticks and lots can hold. All three mean the
	 * @c symbol_spec and the feed disagree about the instrument.
	 *
	 * @warning While this is non-zero the class's central invariant is weaker
	 *          than advertised: the engine's aggregate depth equals @c
	 * replica() *except* at the levels counted here, and the mirror adopts the
	 *          replica regardless, so the divergence does not self-heal. That
	 * is the honest behaviour for a misconfigured listing - the alternative is
	 * emitting a mis-priced order - but it is why this counter exists to be
	 * watched rather than merely available.
	 */
	[[nodiscard]] STRATEGY_EXPORT std::uint64_t dropped_levels() const noexcept;

	/// @brief Lots of seeded depth reported consumed by a match. The size of
	///        the no-market-impact assumption this bridge is running under.
	///        @see consumed
	[[nodiscard]] STRATEGY_EXPORT volume_t consumed_lots() const noexcept;

private:
	/// @brief Append whatever turns @c mirror_ into the replica, then adopt it.
	void emit_resync(std::vector<command> &out);

	/**
	 * @brief Emit the commands taking one side from @p was to @p now.
	 *
	 * Both sides are sorted best-first under the same comparator, so this is
	 * one merge walk: matching prices contribute a delta, a price only in @p
	 * was has been removed, and one only in @p now is new.
	 */
	void diff_side(std::span<const level> was, std::span<const level> now,
				   side_t side, std::vector<command> &out);

	/**
	 * @brief Append the one command moving @p price from @p was to @p now, or
	 *        nothing when they already agree.
	 *
	 * The only place a scaled feed number becomes an engine tick or lot. Both
	 * sizes are converted before they are subtracted, rather than the
	 * difference being converted afterwards: a delta is not guaranteed to sit
	 * on the lot grid even when both endpoints do, and converting it directly
	 * would let a rounding error accumulate against a mirror that never sees
	 * it.
	 */
	void emit_delta(side_t side, market_data::scaled_price_t price,
					market_data::scaled_qty_t was,
					market_data::scaled_qty_t now, std::vector<command> &out);

	/// @brief A scaled size in lots, or nothing when the listing's lot grid
	///        cannot express it.
	/// @note A non-positive size is zero lots, not a failure: that is how an L2
	///       feed says "no level here", and @c quantity_from_scaled rightly
	///       refuses it as an *order* quantity while it is perfectly good as an
	///       endpoint of a delta.
	[[nodiscard]] std::optional<quantity_t>
	to_lots(market_data::scaled_qty_t scaled) const noexcept;

	/// Reference data for the listing: the tick and lot grid every emitted
	/// command is expressed on. Not owned - see the constructor.
	const engine::symbol_spec *spec_;
	symbol_id_t symbol_;
	market_data::depth_reconstructor reconstructor_;
	/// What the engine's book has already been told. Equal to the replica after
	/// every public call.
	market_data::l2_book mirror_;
	std::uint64_t commands_emitted_ = 0;
	std::uint64_t dropped_levels_   = 0;
	/// Seeded depth an identified order took out of the book. @see consumed
	volume_t consumed_lots_ = 0;
};

} // namespace exchange::strategy::backtest
