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
#include "trading-engine/event/command.hpp"
#include "trading-engine/orders/types.hpp"

#include <cstddef>
#include <cstdint>
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
	 * @param symbol The engine-side id every emitted command is addressed to.
	 *        It is this process's reference-data id, not anything the venue
	 *        publishes — the feed says "BTCUSDT", the caller knows which
	 *        @c symbol_spec that is.
	 * @param options Passed to the reconstructor that gap-checks the feed.
	 */
	explicit depth_feed_bridge(
		symbol_id_t symbol,
		market_data::reconstructor_options options = {}) noexcept
		: symbol_(symbol), reconstructor_(options) {}

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
				   side_t side, std::vector<command> &out) const {
		// Best-first means descending for bids and ascending for asks, which is
		// the one place the two sides differ here.
		const auto comes_first = [side](price_t lhs, price_t rhs) noexcept {
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

	/// @brief Append the one command moving @p price from @p was to @p now
	///        lots, or nothing when they already agree.
	void emit_delta(side_t side, price_t price, quantity_t was, quantity_t now,
					std::vector<command> &out) const {
		if (now > was)
			out.push_back(command::add(symbol_, side, price, now - was));
		else if (now < was)
			out.push_back(command::reduce(symbol_, side, price, was - now));
	}

	symbol_id_t symbol_;
	market_data::depth_reconstructor reconstructor_;
	/// What the engine's book has already been told. Equal to the replica after
	/// every public call.
	market_data::l2_book mirror_;
	std::uint64_t commands_emitted_ = 0;
};

} // namespace exchange::app
