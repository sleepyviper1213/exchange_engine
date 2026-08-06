#pragma once
// Where a venue's published depth becomes this engine's resting liquidity.
//
// market-data and trading-engine are siblings and neither links the other: a
// decoder cannot be handed an order_book, and that is enforced by the link
// graph rather than by convention (see docs/directory_layout.md). This header
// is in app/ because app/ is the composition root — the one place allowed to
// name both, and therefore the only place the join can live.
//
// What it is for: seeding a matching book with realistic liquidity taken from a
// live venue, so orders this process originates have something to trade
// against. The depth arrives as anonymous liquidity — engine::order_book's
// add_order rests it under the reserved id 0, uncancellable and unindexed,
// which is exactly what "liquidity nobody owns" should be. It is emphatically
// not a way to reconstruct a venue's book inside the matching engine; l2_book
// already does that, correctly and for a fifth of the memory.

#include "market-data/l2_book.hpp"
#include "market-data/normalised.hpp"
#include "market-data/reconstructor.hpp"
#include "market-data/types.hpp"
#include "trading-engine/event/command.hpp"
#include "trading-engine/orders/types.hpp"
#include "trading-engine/symbol/symbol_spec.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace exchange::app {

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
 * The obvious implementation — read each changed level out of the event and
 * emit the difference — is wrong, because @c depth_reconstructor moves the
 * replica in ways no single event describes. A snapshot reseeds the book and
 * then replays every buffered event onto it. A gap clears it outright. A level
 * falling out of the retained window disappears with no event naming it. Each
 * would leave the engine's book holding depth the venue no longer publishes,
 * and the divergence is silent and permanent.
 *
 * So the rule here is a single one that covers every case: hold a @c mirror_ of
 * what the engine has already been told, and after any operation emit whatever
 * turns the mirror into the replica. The invariant is checkable — once the
 * commands drain, the engine's aggregate depth equals @c replica() — and it
 * holds through gaps, resyncs and evictions without any of them being special
 * cases.
 *
 * The cost is a walk of both books per call rather than of the changed levels
 * alone. Both are contiguous, price-sorted, best-first, so it is a linear merge
 * over two arrays that share a comparator — for the depth an L2 window retains,
 * cheaper than the branchier alternative and considerably easier to be sure of.
 *
 * @warning A gap emits REDUCE for the whole book, and it must. When the replica
 *          dies, liquidity seeded from it is no longer evidence about the
 *          venue, and matching against it would be matching against a snapshot
 *          of the past. Orders this process originated are untouched — they are
 *          identified, and only the anonymous depth is withdrawn.
 *
 * @note Not thread-safe, and single-producer by construction: one feed, one
 *       listing, one bridge, feeding one command stream.
 */
class depth_feed_bridge {
public:
	using command = engine::event::command;
	using level   = market_data::l2_book::Level;

	/**
	 * @brief Bridge the feed for one listing.
	 *
	 * @param spec The listing's trading conventions. It supplies both the
	 *        engine-side id every command is addressed to and — the reason it is
	 *        needed rather than just the id — the tick and lot grid that turns
	 *        the feed's scaled decimals into the engine's ticks and lots. Must
	 *        outlive the bridge; reference data is owned by the registry and
	 *        changes between sessions, not between frames.
	 * @param options Passed to the reconstructor that gap-checks the feed.
	 */
	explicit depth_feed_bridge(
		const engine::symbol_spec &spec,
		market_data::reconstructor_options options = {}) noexcept
		: spec_(&spec), symbol_(spec.id()), reconstructor_(options) {}

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
	market_data::sequence_action on_event(market_data::depth_event event,
										  std::vector<command> &out) {
		const auto action = reconstructor_.on_event(std::move(event));
		emit_resync(out);
		return action;
	}

	/**
	 * @brief Seed or repair from a snapshot; append the commands it implies.
	 *
	 * @param snapshot The full depth; consumed.
	 * @param out Commands are appended, never cleared.
	 * @return Whether the replica is live afterwards. When it is not, a newer
	 *         snapshot is needed and nothing has been seeded.
	 */
	bool on_snapshot(market_data::book_snapshot snapshot,
					 std::vector<command> &out) {
		const bool live = reconstructor_.on_snapshot(std::move(snapshot));
		emit_resync(out);
		return live;
	}

	/**
	 * @brief Declare the replica stale — a transport reconnect, a dropped
	 *        frame — and withdraw the depth it seeded.
	 */
	void invalidate(std::vector<command> &out) {
		reconstructor_.invalidate();
		emit_resync(out);
	}

	/// @brief The listing every emitted command is addressed to.
	[[nodiscard]] symbol_id_t symbol() const noexcept { return symbol_; }

	/// @brief The venue replica. Meaningful only while @c live().
	[[nodiscard]] const market_data::l2_book &replica() const noexcept {
		return reconstructor_.book();
	}

	/**
	 * @brief The depth the engine's book has already been told about.
	 *
	 * Equal to @c replica() after every call — that is the invariant this class
	 * maintains. Exposed so a test, or an operator, can assert it rather than
	 * take it on trust.
	 */
	[[nodiscard]] const market_data::l2_book &mirror() const noexcept {
		return mirror_;
	}

	/// @brief Whether the feed is seeded and in sequence.
	[[nodiscard]] bool live() const noexcept { return reconstructor_.live(); }

	/// @brief Whether the caller owes this bridge a snapshot fetch.
	[[nodiscard]] bool needs_snapshot() const noexcept {
		return reconstructor_.needs_snapshot();
	}

	/// @brief Note that a snapshot fetch is in flight. @see
	///        depth_reconstructor::snapshot_requested
	void snapshot_requested() noexcept {
		reconstructor_.snapshot_requested();
	}

	/// @brief Note that the in-flight fetch failed.
	void snapshot_failed() noexcept { reconstructor_.snapshot_failed(); }

	/// @brief The reconstructor, for its feed-health counters.
	[[nodiscard]] const market_data::depth_reconstructor &
	reconstructor() const noexcept {
		return reconstructor_;
	}

	/// @brief Commands emitted since construction — how much book churn the
	///        feed has cost the engine.
	[[nodiscard]] std::uint64_t commands_emitted() const noexcept {
		return commands_emitted_;
	}

	/**
	 * @brief Level changes the listing's own spec could not express, and which
	 *        were therefore not passed to the engine.
	 *
	 * Should be zero, and a non-zero reading is a configuration fault rather
	 * than a market event — the same kind of number as
	 * @c engine_partition::misrouted. It counts a venue price that is not on
	 * this listing's tick grid, a size not on its lot grid, or either one past
	 * what the engine's 32-bit ticks and lots can hold. All three mean the
	 * @c symbol_spec and the feed disagree about the instrument.
	 *
	 * @warning While this is non-zero the class's central invariant is weaker
	 *          than advertised: the engine's aggregate depth equals @c replica()
	 *          *except* at the levels counted here, and the mirror adopts the
	 *          replica regardless, so the divergence does not self-heal. That is
	 *          the honest behaviour for a misconfigured listing — the
	 *          alternative is emitting a mis-priced order — but it is why this
	 *          counter exists to be watched rather than merely available.
	 */
	[[nodiscard]] std::uint64_t dropped_levels() const noexcept {
		return dropped_levels_;
	}

private:
	/// @brief Append whatever turns @c mirror_ into the replica, then adopt it.
	void emit_resync(std::vector<command> &out) {
		const std::size_t before = out.size();
		const market_data::l2_book &live_book = reconstructor_.book();

		diff_side(mirror_.bid_levels(), live_book.bid_levels(), side_t::bid, out);
		diff_side(mirror_.ask_levels(), live_book.ask_levels(), side_t::ask, out);

		// Adopt after diffing, never before. load() installs both sides
		// wholesale from storage the mirror already owns, so this allocates
		// nothing.
		mirror_.load(side_t::bid, live_book.bid_levels());
		mirror_.load(side_t::ask, live_book.ask_levels());

		commands_emitted_ += out.size() - before;
	}

	/**
	 * @brief Emit the commands taking one side from @p was to @p now.
	 *
	 * Both sides are sorted best-first under the same comparator, so this is one
	 * merge walk: matching prices contribute a delta, a price only in @p was has
	 * been removed, and one only in @p now is new.
	 */
	void diff_side(std::span<const level> was, std::span<const level> now,
				   side_t side, std::vector<command> &out) {
		// Best-first means descending for bids and ascending for asks, which is
		// the one place the two sides differ here. Both spans are the venue's
		// scaled prices — the conversion to ticks happens once, in emit_delta,
		// after the merge has decided what actually changed.
		const auto comes_first = [side](market_data::scaled_price_t lhs,
										market_data::scaled_price_t rhs) noexcept {
			return side == side_t::bid ? lhs > rhs : lhs < rhs;
		};

		std::size_t old_at = 0;
		std::size_t new_at = 0;
		while (old_at < was.size() && new_at < now.size()) {
			const level &old_level = was[old_at];
			const level &new_level = now[new_at];
			if (old_level.price == new_level.price) {
				emit_delta(side, old_level.price, old_level.qty, new_level.qty,
						   out);
				++old_at;
				++new_at;
			} else if (comes_first(old_level.price, new_level.price)) {
				// The venue no longer publishes this price at all.
				emit_delta(side, old_level.price, old_level.qty, 0, out);
				++old_at;
			} else {
				emit_delta(side, new_level.price, 0, new_level.qty, out);
				++new_at;
			}
		}
		for (; old_at < was.size(); ++old_at)
			emit_delta(side, was[old_at].price, was[old_at].qty, 0, out);
		for (; new_at < now.size(); ++new_at)
			emit_delta(side, now[new_at].price, 0, now[new_at].qty, out);
	}

	/**
	 * @brief Append the one command moving @p price from @p was to @p now, or
	 *        nothing when they already agree.
	 *
	 * The only place a scaled feed number becomes an engine tick or lot. Both
	 * sizes are converted before they are subtracted, rather than the difference
	 * being converted afterwards: a delta is not guaranteed to sit on the lot
	 * grid even when both endpoints do, and converting it directly would let a
	 * rounding error accumulate against a mirror that never sees it.
	 */
	void emit_delta(side_t side, market_data::scaled_price_t price,
					market_data::scaled_qty_t was,
					market_data::scaled_qty_t now, std::vector<command> &out) {
		if (was == now) return;

		const auto ticks    = spec_->price_from_scaled(price);
		const auto was_lots = to_lots(was);
		const auto now_lots = to_lots(now);
		if (!ticks.has_value() || !was_lots.has_value() ||
			!now_lots.has_value()) {
			// The feed and the listing's spec disagree. Emitting anything here
			// would be inventing a price or a size the venue never published.
			++dropped_levels_;
			return;
		}

		if (*now_lots > *was_lots)
			out.push_back(
				command::add(symbol_, side, *ticks, *now_lots - *was_lots));
		else if (*now_lots < *was_lots)
			out.push_back(
				command::reduce(symbol_, side, *ticks, *was_lots - *now_lots));
	}

	/// @brief A scaled size in lots, or nothing when the listing's lot grid
	///        cannot express it.
	/// @note A non-positive size is zero lots, not a failure: that is how an L2
	///       feed says "no level here", and @c quantity_from_scaled rightly
	///       refuses it as an *order* quantity while it is perfectly good as an
	///       endpoint of a delta.
	[[nodiscard]] std::optional<quantity_t>
	to_lots(market_data::scaled_qty_t scaled) const noexcept {
		if (scaled <= 0) return quantity_t{0};
		const auto lots = spec_->quantity_from_scaled(scaled);
		if (!lots.has_value()) return std::nullopt;
		return *lots;
	}

	/// Reference data for the listing: the tick and lot grid every emitted
	/// command is expressed on. Not owned — see the constructor.
	const engine::symbol_spec *spec_;
	symbol_id_t symbol_;
	market_data::depth_reconstructor reconstructor_;
	/// What the engine's book has already been told. Equal to the replica after
	/// every public call.
	market_data::l2_book mirror_;
	std::uint64_t commands_emitted_ = 0;
	std::uint64_t dropped_levels_   = 0;
};

} // namespace exchange::app
