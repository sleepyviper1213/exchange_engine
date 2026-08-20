#pragma once
// What the account is actually holding, published so anyone may read it.
//
// This is the one genuinely shared structure in the risk module, and the only
// reason there are atomics anywhere in it.

#include "detail/position_entry.hpp"
#include "risk_management_export.hpp" // RISK_MANAGEMENT_EXPORT (generated)
#include "trading-engine/orders/types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace exchange::risk::hooks::pre_trade {

/**
 * @brief One listing's position, read all at once.
 *
 * @warning Each field is read atomically; the *set* is not. A snapshot taken
 *          while a fill is being applied can show the new @c net_lots beside
 *          the old @c net_notional. That is deliberate - making it consistent
 *          means a seqlock, and a seqlock means the writer pays two stores and
 *          a fence on the fill path to serve a reader that only ever draws a
 *          dashboard. Nothing in the gate reads two fields as a pair, so
 *          nothing in the gate can be wrong about it.
 */
struct position_snapshot {
	volume_t net_lots         = 0; ///< signed: positive is long
	std::int64_t net_notional = 0; ///< signed sum of price*qty, in tick-lots
	volume_t bought_lots      = 0; ///< lifetime buys, for turnover
	volume_t sold_lots        = 0; ///< lifetime sells
	volume_t working_bid_lots = 0; ///< quantity resting or in flight to buy
	volume_t working_ask_lots = 0; ///< quantity resting or in flight to sell

	/**
	 * @brief Lots at risk if every working order filled and the position had to
	 *        be closed - the number a gross limit is measured against.
	 *
	 * The worse of the two directions rather than their sum: a long position
	 * with working sells is being *closed* by them, so adding the two would
	 * charge an account for reducing its risk. Taking the max charges it for
	 * whichever side could actually grow.
	 */
	[[nodiscard]] RISK_MANAGEMENT_EXPORT volume_t gross_lots() const noexcept;

	/**
	 * @brief Realised plus unrealised profit at @p mark, in tick-lots.
	 *
	 * @par Why this needs no extra state
	 * @c net_notional is the signed cash the account has *spent* - a buy adds
	 * its notional, a sell subtracts it - and @c net_lots is what that cash
	 * bought. So what the position is worth now is @c net_lots * @c mark, what
	 * it cost is
	 * @c net_notional, and the difference is the whole profit. Both halves fall
	 * out, with no separate realised bucket and no average-price bookkeeping to
	 * drift.
	 *
	 * Buy 10 at 100 and mark at 110: @c 10*110 - 1000 = +100, unrealised. Sell
	 * those 10 at 110 and the position is flat with @c net_notional == -100, so
	 * the answer is @c 0*mark + 100 - the same +100, now realised, and the mark
	 * has stopped mattering. That transition being free is the point.
	 *
	 * @param mark Price to value the open position at, in ticks. Usually the
	 *        last print.
	 * @return Signed tick-lots. Negative is a loss.
	 *
	 * @warning Tick-lots, so it is comparable across time on one listing and
	 * not across listings. @see risk_limits on why nothing here converts.
	 */
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::int64_t
	pnl(price_t mark) const noexcept;
};

/**
 * @brief Per-listing position for one account, written by one thread and
 *        readable by any.
 *
 * @par The concurrency contract, stated exactly
 * **One writer per symbol, any number of readers.** In this engine the writer
 * is the thread that runs a listing's strategy host and its gate - the same
 * thread that receives the partition's published trades, so it is the only
 * thread that ever learns a fill happened. Readers are everything else: a risk
 * dashboard, a firm-wide aggregator, an operator deciding whether to trip the
 * breaker.
 *
 * @par Why that contract buys a faster update than @c fetch_add
 * With one writer, a read-modify-write does not have to be *atomic* - nobody
 * else can interleave with it. It only has to be *race-free*, which a relaxed
 * load followed by a relaxed store already is. So the update compiles to
 * `mov / add / mov` with no @c lock prefix: about a nanosecond, against roughly
 * twenty for a `lock xadd` that also serialises the store buffer. On a path
 * that runs per execution, that is the difference between free and noticeable.
 *
 * Readers still see whole values - the loads and stores are atomic, so there is
 * no tearing on any of these 64-bit fields - they just see them one at a time.
 * @see position_snapshot for what that costs a reader.
 *
 * @par Why the ordering is relaxed, and why that is not laziness
 * Relaxed is the whole ordering requirement here, because every counter is
 * independently meaningful and none of them publishes anything else. There is
 * no "write the data then release the flag" pattern to protect: the counter
 * *is* the data. What a reader needs is that the value it loads was some value
 * the writer wrote and that the writer's stores become visible in bounded time,
 * and cache coherence provides both without any fence.
 *
 * The consequence is that a gate may screen an order against a position that is
 * one fill stale. That is not a correctness hole, it is a sizing decision: a
 * limit is a threshold on a quantity that is already changing while you check
 * it, so it is checked with a buffer, and the buffer is measured in orders
 * rather than in nanoseconds of staleness. Paying for sequential consistency
 * would narrow the window and not close it.
 *
 * @par Layout
 * One cache line per listing, so two threads updating two symbols never share
 * one. Six 8-byte counters is 48 bytes, which fits with room to spare; the
 * padding is the point and is not waste.
 *
 * @note Capacity is fixed at construction and the storage is allocated once, at
 *       startup. Nothing here allocates afterwards, which is the invariant the
 *       ingest path depends on.
 */
class position_book {
public:
	/// @brief Listings a default book carries. Dense symbol ids index it
	///        directly, so this is a highest-id bound, not a count of what is
	///        in use.
	static constexpr std::size_t DEFAULT_CAPACITY = 1024;

	RISK_MANAGEMENT_EXPORT explicit position_book(
		std::size_t capacity = DEFAULT_CAPACITY);

	/**
	 * @brief Neither copied nor moved, and the deletion is load-bearing twice.
	 *
	 * The design reason: a book is what several threads agree on, and every
	 * gate built over one holds a pointer to it. Relocating it out from under
	 * them would leave the risk checks reading freed storage, and duplicating
	 * it would leave two accounts each convinced it holds the whole position.
	 *
	 * It is also what the storage says: the counters are atomics, so copying
	 * the vector that holds them is ill-formed anyway. Declaring that outright
	 * beats leaving a copy that only fails once somebody writes it.
	 */
	position_book(const position_book &)            = delete;
	position_book &operator=(const position_book &) = delete;
	position_book(position_book &&)                 = delete;
	position_book &operator=(position_book &&)      = delete;
	~position_book()                                = default;

	/// @brief How many listings this book can hold.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::size_t capacity() const noexcept;

	/// @brief Whether @p symbol is inside this book's range.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT bool
	carries(symbol_id_t symbol) const noexcept;

	// --- writer side: one thread per symbol -------------------------------

	/**
	 * @brief Record an execution of @p lots at @p price on @p side.
	 *
	 * @param symbol The listing. @pre @c carries(symbol).
	 * @param side Which way *this account* traded - bid means it bought.
	 * @param price Execution price in ticks, which is the resting order's price
	 *        and not necessarily the aggressor's limit.
	 * @param lots Executed quantity. @pre positive.
	 *
	 * @note A self-trade - this account on both sides of one print - is applied
	 *       twice, once per side, and nets to zero. That is the right answer
	 * and it falls out rather than being special-cased.
	 */
	RISK_MANAGEMENT_EXPORT void apply_fill(symbol_id_t symbol, side_t side,
										   price_t price,
										   quantity_t lots) noexcept;

	/// @brief Note that @p lots have been sent to the book on @p side and are
	///        not yet done - the exposure a limit must count before any fill.
	///
	/// @note Takes @c volume_t rather than @c quantity_t because a caller
	///       publishes a whole batch's worth at once, and a batch can hold more
	///       lots than any one order may.
	RISK_MANAGEMENT_EXPORT void add_working(symbol_id_t symbol, side_t side,
											volume_t lots) noexcept;

	/// @brief Note that @p lots on @p side are no longer working - filled,
	///        cancelled, or refused by the book.
	RISK_MANAGEMENT_EXPORT void remove_working(symbol_id_t symbol, side_t side,
											   volume_t lots) noexcept;

	/// @brief Forget everything about @p symbol. A session boundary, not
	///        something to do while orders are working.
	RISK_MANAGEMENT_EXPORT void reset(symbol_id_t symbol) noexcept;

	// --- reader side: any thread ------------------------------------------

	/// @brief Signed net position in lots. Positive is long.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT volume_t
	net_lots(symbol_id_t symbol) const noexcept;

	/// @brief Working quantity on @p side, in lots.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT volume_t
	working_lots(symbol_id_t symbol, side_t side) const noexcept;

	// No `pnl(symbol, mark)` overload here, deliberately: both parameters are
	// 32-bit unsigned, so `pnl(mark, symbol)` would compile and be wrong -
	// clang-tidy flags exactly that. `snapshot(symbol).pnl(mark)` cannot be
	// transposed, reads better, and costs four extra loads off the same cache
	// line the two it needs are already on.

	/// @brief Every counter for @p symbol. @see position_snapshot on
	///        field-wise, not set-wise, atomicity.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT position_snapshot
	snapshot(symbol_id_t symbol) const noexcept;

private:
	/// @brief One cache line per listing. The counters, the padding around them
	///        and the single-writer update all live with the storage, in
	///        @c detail/position_entry.hpp - none of it is anything a caller of
	///        this class can name.
	std::vector<detail::position_entry> entries_;
};

} // namespace exchange::risk::hooks::pre_trade