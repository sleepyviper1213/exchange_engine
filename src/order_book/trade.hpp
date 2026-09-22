#pragma once
#include "orders/types.hpp"

#include <type_traits>

namespace exchange::engine {

/**
 * @brief One execution produced by matching, and the whole public account of
 *        it.
 *
 * @note Trades always print at the resting (passive) order's price.
 *
 * @par Why this is the record and not half of one
 * A venue owes each side of an execution its own report, and the obvious way to
 * give it is to publish a fill per side - which is what Emporia's
 * @c publishFills does. This does not, because everything such a pair would
 * carry is here already: the taker is @c aggressor on @c aggressor_side, the
 * maker is @c resting on @c opposed(aggressor_side), and both report the same
 * @c price and @c volume. Splitting the record would double the hot-path writes
 * to say the same thing twice.
 *
 * What the split really buys is *per-order state* - the maker's remaining
 * quantity after this fill - and that is not a property of the execution at
 * all. It comes off @c order_outcome, which the matching loop already emits per
 * side, and the two are joined by @c id. @see order_outcome::trade_id
 *
 * @par Identity
 * @c id names the execution within its listing and @c sequence names the
 * command that caused it. Both are assigned on the way out of the engine, not
 * by whoever built the order, so neither can disagree with the journal.
 * @see trade_id_t, engine_sequence_t
 *
 * @note 56 bytes, which puts a trade and the 8-byte routing header
 *       @c event::engine_event wraps it in at exactly one cache line. That is
 *       the budget: this is the record that leaves the process, so it carries
 *       what a downstream tape needs and nothing beyond it.
 * @par Which fields default
 * The four identity fields do and the four execution fields do not, which is
 * one rule stated twice: a field whose zero has a defined meaning declares that
 * zero, and a field that must be stated stays undefaulted. So a trade built by
 * hand - a test, a benchmark, the venue print @c session::live_session
 * synthesises - says what executed and stays silent about an identity it does
 * not have, rather than restating three zeros and a side. And a @c trade
 * declared without an initialiser reads as unassigned rather than as whatever
 * was on the stack. @c aggressor_side is the awkward one; its note says why.
 */
struct trade {
	// The first four fields are the execution itself and keep their order:
	// every call site that builds a trade by hand names them positionally or in
	// this order through designated initialisers.
	order_id_t aggressor; ///< id of the incoming, aggressing order
	order_id_t resting;   ///< id of the passive order that was hit
	price_t price;        ///< execution price (the resting order's price)
	quantity_t volume;    ///< executed quantity

	/// @brief This listing's execution number, from 1. Zero means the trade was
	///        built by hand rather than printed by a book. @see trade_id_t
	trade_id_t id = 0;

	/// @brief The command this execution came out of. @see engine_sequence_t
	engine_sequence_t sequence = 0;

	/**
	 * @brief When the aggressor reached the venue, in nanoseconds since the
	 *        Unix epoch - **not** when the match happened.
	 *
	 * Carried off @c order::timestamp, which the gateway stamped, rather than
	 * read from a clock here. Two reasons, and the second is the one that
	 * decides it.
	 *
	 * A clock read is tens of nanoseconds against a matching path budgeted in
	 * nanoseconds, and it would be paid per execution - a sweep of forty levels
	 * would read the clock forty times to record forty values that differ by
	 * less than the cost of reading them.
	 *
	 * And a clock read is not deterministic. Replaying a journal must reproduce
	 * the live run record for record, which is the property recovery and
	 * differential testing both rest on; a timestamp taken at match time makes
	 * every replayed trade differ from the one it is meant to reproduce.
	 * Receipt time is in the journal, so it replays exactly.
	 *
	 * @note So this measures a client's round trip, not the engine's matching
	 *       latency. The latter is @c partition_metrics::drain_latency_ns.
	 */
	timestamp_t timestamp = 0;

	/**
	 * @brief Which side the aggressor was on - the direction of the print.
	 *
	 * An uptick is an aggressive buy. Without this a consumer can only infer
	 * direction by comparing consecutive prices, which is wrong at a level that
	 * trades twice and undefined for the first print of a session.
	 *
	 * @note It says which side *took* liquidity, not which side was ours. A
	 *       participant may be either the aggressor or the resting order, and
	 *       only the order ids say which.
	 * @note The one identity field with no "unassigned" value to default to -
	 *       @c side_t is bool-backed and has two enumerators, both meaningful.
	 *       Widening it for this would put a third case in every switch on the
	 *       matching path to serve one that cannot arise there, so a trade
	 *       carrying no @c id carries no trustworthy direction either.
	 */
	side_t aggressor_side = side_t::bid;

	bool operator==(const trade &) const noexcept = default;
};

static_assert(std::is_trivially_copyable_v<trade>,
			  "trade must stay trivially copyable: it travels the queue's "
			  "memcpy batch path inside event::engine_event");
static_assert(sizeof(trade) == 56,
			  "a trade plus engine_event's 8-byte routing header is budgeted "
			  "at one cache line");

} // namespace exchange::engine
