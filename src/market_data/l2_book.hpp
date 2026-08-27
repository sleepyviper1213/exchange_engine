#pragma once
#include "core/util/attributes.hpp"
#include "depth_sweep.hpp"
#include "fwd.hpp"
#include "market_data_export.hpp"
#include "orders/types.hpp"
#include "types.hpp" // scaled_price_t / scaled_qty_t

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>

namespace exchange::market_data {

/**
 * @brief A cache-optimised Level-2 (aggregate-by-price) book for the managed
 *        local-order-book reconstruction path.
 *
 * Each side is a single contiguous, price-sorted array of {price, qty} cells
 * - bids descending, asks ascending, so the best price is always @c front().
 * This is market data's own view of the depth an exchange @em publishes: the L2
 * diff feed only ever carries an absolute aggregate size per price, so a flat
 * array is all that is needed and all that should be paid for. @c set_level is
 * a binary search plus an in-place qty write (or a shift on insert/erase),
 * best bid/ask is @c front(), and a top-of-book walk is sequential over
 * contiguous memory with no per-level pointer chase. At 16 bytes per cell, four
 * levels share a cache line.
 *
 * This is a reconstruction / quote book: it models absolute L2 sizes (a size of
 * 0 removes the price) and deliberately does @b not match, track order
 * identity, or model FIFO priority. Those belong to @c engine::order_book, the
 * trading engine's order-by-order (L3) book that keeps a FIFO of individual @c
 * order objects per level - a different concept in a different subsystem. Do
 * not mix this with @c order_book's place_order()/cancel_order() flow.
 */
class l2_book {
public:
	/// @brief One aggregated price level: a price and the total size resting on
	///        it. Trivially copyable and 16 bytes so a side packs densely.
	struct price_level {
		scaled_price_t price;
		scaled_qty_t qty;
	};

	/// @brief Levels per side when the caller does not choose. Comfortably
	///        above the 10-50 a top-of-book consumer reads, and 2 KB per side.
	static constexpr std::size_t DEFAULT_DEPTH = 128;

	/**
	 * @brief Construct a book retaining exactly @p max_depth levels per side.
	 *
	 * @par The depth is fixed, and that is the point
	 * One allocation happens here, sized for both sides, and the book never
	 * takes another for as long as it lives. No path - not @c set_level, not
	 * @c load, not @c clear - can reach the allocator, so the update path has
	 * no reallocation to be surprised by: no unbounded copy, no latency spike
	 * when a side happens to outgrow its capacity, and no dependence on how the
	 * allocator is feeling. The cells also stay put, so a pointer or span into
	 * a side stays valid until the book is destroyed.
	 *
	 * The cap does a second job: @c set_level's insert and erase paths memmove
	 * the tail of a side, so their cost is linear in retained depth while the
	 * overwrite path is flat. A book that keeps 1000 levels pays that shift on
	 * every new price near the touch - which is where a diff feed puts almost
	 * all of them - and a consumer that only ever reads the top 10-50 levels
	 * pays it for depth it never looks at. Bounding the side bounds the shift.
	 *
	 * @par What a fixed depth costs
	 * A bounded book is a @b top-N view, not an exact replica, and there is no
	 * longer an "unbounded" setting to escape to - retaining every level a
	 * venue publishes and never reallocating are contradictory requirements,
	 * and this class now picks the second. An L2 diff feed only reports prices
	 * whose size changed, so once a level falls outside the window its size is
	 * forgotten and cannot be recovered from the stream; the venue will not
	 * resend it until it changes again. Beyond the window @c volume_at_price
	 * returns 0 for "outside the retained view" exactly as it does for "no
	 * level here", and the two are indistinguishable.
	 *
	 * Choose a depth comfortably above what any consumer reads, and watch
	 * @c dropped_levels to find out whether you did.
	 *
	 * @param max_depth Levels retained per side. Must be positive.
	 */
	MARKET_DATA_EXPORT explicit l2_book(std::size_t max_depth = DEFAULT_DEPTH);

	/// Move-only: the cells are one owned block, and copying a book is not
	/// something any call site in the tree wants to do by accident.
	l2_book(l2_book &&) noexcept            = default;
	l2_book &operator=(l2_book &&) noexcept = default;
	l2_book(const l2_book &)                = delete;
	l2_book &operator=(const l2_book &)     = delete;
	~l2_book()                              = default;

	/**
	 * @brief Set the absolute aggregate size at @p price on @p side.
	 *
	 * The L2 diff primitive: a @c qty <= 0 removes the level; otherwise the
	 * level is created (in sorted position) or its size overwritten. O(1) to
	 * update an existing level; O(log n) search plus a shift to insert or
	 * erase, bounded by @c max_depth because the side cannot grow past it.
	 *
	 * That shift is the expensive path and clustering does @b not make it
	 * cheap. A side is stored best-first, so a new price near the touch shifts
	 * nearly every level behind it while the worst price shifts none - the
	 * top-of-book concentration a diff feed exhibits lands its inserts on the
	 * maximum-shift end, not the cheap one. Measured (order_latency, 1000
	 * levels/side, p99): ~38 ns to overwrite, 300-390 ns to insert near the
	 * touch. Depth is what the shift is linear in, which is what @c max_depth
	 * exists to bound.
	 */
	MARKET_DATA_EXPORT void set_level(side_t side, scaled_price_t price,
									  scaled_qty_t volume);

	/**
	 * @brief Replace @p side's levels wholesale with @p levels - the snapshot
	 *        seed path.
	 *
	 * Reads @p levels and puts the side straight into its invariant: levels
	 * with a non-positive size dropped (an absent price and a zero-size price
	 * are the same state), sorted best-first, and at most one level per price.
	 * The caller therefore need not know how a venue orders a snapshot, which
	 * is the point - feeding the same levels through @c set_level one at a time
	 * costs a shift per insert in whatever order the venue happens not to use.
	 *
	 * A @c span rather than a @c vector by value: the caller keeps its buffer
	 * and this selects the best @c max_depth levels straight into storage that
	 * already exists, so the snapshot path takes no allocation either. When the
	 * snapshot is deeper than the book, the surplus is counted in
	 * @c dropped_levels rather than silently discarded.
	 *
	 * @param side The side to replace.
	 * @param levels The side's complete depth, in any order.
	 * @note A duplicated price keeps the first occurrence. A well-formed
	 *       snapshot has none; one that does would otherwise break the binary
	 *       search every other operation relies on.
	 */
	MARKET_DATA_EXPORT void load(side_t side,
								 std::span<const price_level> levels);

	/// @brief Overload for a braced list of levels, so a literal snapshot in a
	///        test or a seed reads the same as one from the wire.
	MARKET_DATA_EXPORT void load(side_t side,
								 std::initializer_list<price_level> levels);

	/// @brief Drop every level on both sides. The storage stays where it is.
	MARKET_DATA_EXPORT void clear() noexcept;

	/// @brief Best (highest) bid price, or std::nullopt if no bids rest.
	[[nodiscard]] MARKET_DATA_EXPORT std::optional<scaled_price_t>
	best_bid() const noexcept;

	/// @brief Best (lowest) ask price, or std::nullopt if no asks rest.
	[[nodiscard]] MARKET_DATA_EXPORT std::optional<scaled_price_t>
	best_ask() const noexcept;

	/**
	 * @brief Is the best bid at or above the best ask?
	 *
	 * A consistency check the sequence numbers structurally cannot provide.
	 * @c depth_sequencer proves that every update arrived, once, in order; it
	 * says nothing about whether the resulting book means anything. A crossed
	 * book is the classic symptom of the failures that leave the sequence
	 * intact - a torn REST snapshot, a side mixed up in a decoder, a venue
	 * publishing garbage - so checking it is the cheapest independent evidence
	 * available that reconstruction is actually working.
	 *
	 * Capping cannot cause a false positive: a capped side drops its @em worst
	 * levels and always keeps the touch, so a cross detected here is a cross
	 * that really exists in the retained view.
	 *
	 * @note Locked (bid == ask) counts as crossed. Some venues publish a
	 *       momentarily locked book legitimately; for a continuous-matching
	 *       venue it should not survive a completed event, and treating it as
	 *       suspect is the same fail-loudly posture as everything else on this
	 *       path. @c reconstructor_options::resync_on_cross turns the reaction
	 *       off for a venue where it is normal.
	 * @warning Meaningful only between events, never inside one. @c apply
	 *          writes a whole event's bids before its asks, so a book can be
	 *          transiently crossed part-way through an event that leaves it
	 *          perfectly consistent.
	 */
	[[nodiscard]] MARKET_DATA_EXPORT bool is_crossed() const noexcept;

	/// @brief Aggregate size at @p price on @p side, or 0 if no level rests
	///        there.
	[[nodiscard]] MARKET_DATA_EXPORT scaled_qty_t
	volume_at_price(scaled_price_t price, side_t side) const;

	/// @brief Number of resting levels on @p side.
	[[nodiscard]] MARKET_DATA_EXPORT std::size_t
	depth(side_t side) const noexcept;

	/// @brief The per-side retention cap, fixed at construction.
	[[nodiscard]] MARKET_DATA_EXPORT std::size_t max_depth() const noexcept;

	/**
	 * @brief Levels the window refused or evicted since construction.
	 *
	 * The cost of the cap, made countable. Every level that a deeper book would
	 * have kept is counted here exactly once: one per @c set_level that landed
	 * outside a full window or pushed the worst level out of it, and the
	 * surplus of every @c load deeper than @c max_depth. A book sized right for
	 * its feed reports a small and stable number; one climbing steadily is
	 * throwing away depth its consumers may be reading as zero.
	 */
	[[nodiscard]] MARKET_DATA_EXPORT std::uint64_t
	dropped_levels() const noexcept;

	/**
	 * @brief The bid side, best (highest) price first.
	 *
	 * Reads name their side rather than taking a @c side_t, matching
	 * @c best_bid / @c best_ask. Nothing crosses sides on a reconstruction book
	 * - it does not match, so it never needs @c opposed() - and every read call
	 * site in the tree knows its side at compile time, so a parametric reader
	 * would only add a branch to undo one the caller had already resolved.
	 *
	 * The mutating half stays parametric: @c set_level and @c load have callers
	 * carrying a genuinely runtime side (the streaming decoder walking the bid
	 * then ask array, and the matching engine applying a command off the wire).
	 *
	 * @note A @c span, not a container reference: the cells are a window into a
	 *       block the book owns, and there is no container object to hand out.
	 *       It stays valid for the book's lifetime - the storage never moves -
	 *       but its @c size() changes as levels come and go.
	 */
	[[nodiscard]] MARKET_DATA_EXPORT std::span<const price_level>
	bid_levels() const noexcept EXCHANGE_LIFETIMEBOUND;

	/// @brief The ask side, best (lowest) price first. @see bid_levels
	[[nodiscard]] MARKET_DATA_EXPORT std::span<const price_level>
	ask_levels() const noexcept EXCHANGE_LIFETIMEBOUND;

	/**
	 * @brief What taking @p size from the asks would cost - the buyer's side.
	 *
	 * Walks the published depth from the touch outwards, the way an aggressive
	 * order would consume it, and reports how much of @p size is actually
	 * there, how many levels it reaches through, and where that leaves the
	 * touch.
	 * @see depth_sweep, which explains why the answer is four numbers and not a
	 *      single average price.
	 *
	 * @par Why this is on the book rather than left to the caller
	 * Because the level spans above already let anybody write the loop, and
	 * that is the problem: every consumer that wants a price-impact estimate
	 * writes the same walk, and each one gets to be subtly wrong on its own - a
	 * partial last level counted whole, a level counted when the size ran out
	 * exactly at its boundary, an empty side read as a free fill. The walk is
	 * short enough to be worth having exactly once, next to the invariants it
	 * depends on.
	 *
	 * @param size Size wanted, in the feed's scaled units. Non-positive returns
	 *        an empty sweep rather than an error - "take nothing" has an
	 * answer.
	 * @return The decomposition. @c is_complete is false when the retained
	 *         window ran out before @p size did, which is a real possibility on
	 *         a capped book and is *not* distinguishable from a genuinely thin
	 *         venue. @see max_depth and dropped_levels - a book whose window is
	 *         too small reports pessimistic impact, and this is one of the
	 * reads that makes the cap visible.
	 *
	 * @note Named for the side consumed, matching @c ask_levels rather than the
	 *       intent of the caller: a *buyer* calls this one.
	 */
	[[nodiscard]] MARKET_DATA_EXPORT depth_sweep
	sweep_asks(scaled_qty_t size) const noexcept;

	/// @brief What taking @p size from the bids would cost - the seller's side.
	/// @see sweep_asks for everything the two share, which is everything but
	///      the direction prices get worse in.
	[[nodiscard]] MARKET_DATA_EXPORT depth_sweep
	sweep_bids(scaled_qty_t size) const noexcept;

	/**
	 * @brief Resting levels across both sides - @c depth(bid) + @c depth(ask).
	 *
	 * Levels, not orders and not volume. An aggregate book has no order
	 * identity to count and the sizes on its cells are quantities rather than a
	 * population, so the only thing there is a number of here is prices with
	 * something resting at them. @c depth is the per-side answer; this is the
	 * whole book, for a caller that just wants to know how much of the window
	 * is in use.
	 *
	 * @note Not capacity. The book can hold @c 2 * max_depth() levels, and this
	 *       counts the live ones - it is 0 on a freshly constructed book of any
	 *       depth.
	 */
	[[nodiscard]] MARKET_DATA_EXPORT std::size_t size() const noexcept;

	/// @brief True when no level rests on either side - @c size() @c == @c 0.
	/// @note A book that has been @c clear()ed is empty; so is one whose every
	///       level went to size 0 on the wire, since an absent price and a
	///       zero-size price are the same state here.
	[[nodiscard]] MARKET_DATA_EXPORT bool is_empty() const noexcept;


private:
	/// Both sides live in one block, bids first: one allocation instead of two,
	/// and the two sides land adjacent so a book that fits in cache does so as
	/// a unit rather than as two independently placed arrays.
	[[nodiscard]] price_level *bids() noexcept;

	[[nodiscard]] price_level *asks() noexcept;

	[[nodiscard]] const price_level *bids() const noexcept;

	[[nodiscard]] const price_level *asks() const noexcept;

	/// @brief The walk both @c sweep_asks and @c sweep_bids are: consume
	///        @p size from @p levels, which are already in best-first order.
	///
	/// Static and side-parametric because the two public entry points differ
	/// only in which array they hand over and which @c side_t they label the
	/// result with - the arithmetic does not know or care which way prices are
	/// sorted, since best-first is the array's own invariant.
	[[nodiscard]] static depth_sweep sweep(const price_level *levels,
										   std::size_t count, side_t side,
										   scaled_qty_t size) noexcept;

	std::unique_ptr<price_level[]>
		cells_;                 ///< 2 * max_depth_ cells: bids, then asks
	std::size_t max_depth_ = 0;
	std::size_t bid_size_  = 0; ///< descending by price (best = highest = [0])
	std::size_t ask_size_  = 0; ///< ascending  by price (best = lowest  = [0])
	std::uint64_t dropped_levels_ = 0;
};

} // namespace exchange::market_data
