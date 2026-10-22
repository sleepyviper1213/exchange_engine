#pragma once
// What an execution does to the account: the position it moves, the working
// exposure it consumes, and what it cost.
//
// Split out of risk_gate, which used to hold it beside the screening rules. The
// two change for different reasons - every fix to the booking (a venue fill
// arriving after the ledger let the order go, an engine print on a run whose
// fills come from a venue, a commission the position book never saw) came from
// a Demo Mode run disagreeing with the venue's trade history, and none touched
// a pre-trade rule. The gate still owns the mark and the loss floor, because
// the mark is also the centre of the fat-finger band; what is here only books,
// and answers what the booking comes to at a mark it is given.
//
// Header-only, as it was inside the gate: book_engine runs on every print the
// gate sees, and moving it behind a library boundary would add a call there
// that nothing has measured.

#include "orders/outcome.hpp"
#include "orders/trade.hpp"
#include "orders/types.hpp"
#include "position.hpp"
#include "working_ledger.hpp"

#include <cstdint>

namespace exchange::risk::hooks::pre_trade {

/**
 * @brief Books executions and their costs for one listing, against the
 *        ledger the gate screens with and the position book it shares.
 *
 * @par Threading
 * The gate's thread, like the gate. The position book is the shared object
 * and carries its own contract; nothing here adds to it.
 */
class fill_booking {
public:
	/// @param positions Shared; must outlive this and carry @p symbol.
	/// @param ledger The gate's own; must outlive this.
	fill_booking(position_book &positions, working_ledger &ledger,
				 symbol_id_t symbol) noexcept
		: positions_(&positions), ledger_(&ledger), symbol_(symbol) {}

	/**
	 * @brief Book an execution the engine printed, on whichever side of it is
	 *        ours.
	 *
	 * Refused - counted, not booked - once @c set_books_engine_fills is off and
	 * the print names an order: then the venue is the market, and a match the
	 * engine made against our order is one the venue never made. Two one-hour
	 * Demo Mode runs each ended one 50-lot order away from the venue's trade
	 * history with exactly one execution too many.
	 */
	void book_engine(const engine::trade &execution) noexcept {
		if (!is_booking_engine_fills_ && execution.id != 0 &&
			(execution.aggressor != 0 || execution.resting != 0)) {
			++ignored_engine_fills_;
			return;
		}
		apply_side(execution.aggressor, execution);
		apply_side(execution.resting, execution);
	}

	/**
	 * @brief Book an execution the *venue* reports against our order @p id.
	 *
	 * What it takes from the ledger it books on the ledger's terms. What the
	 * ledger no longer has - the order was withdrawn or shrunk before the
	 * venue's fill arrived - it books anyway, on @p side: a venue report names
	 * ours by construction, whereas an engine print naming an order the ledger
	 * does not know is somebody else's trade. A one-hour Demo Mode run lost 430
	 * such fills to the withdraw-while-filling race and ended 46 lots away from
	 * the account.
	 */
	void book_venue(order_id_t id, side_t side, price_t price,
					quantity_t qty) noexcept {
		quantity_t booked{};
		if (const auto taken = ledger_->take(id, qty)) {
			positions_->apply_fill(symbol_, taken->side, price, taken->taken);
			positions_->remove_working(symbol_, taken->side, taken->taken);
			booked = taken->taken;
		}
		if (booked < qty) {
			positions_->apply_fill(symbol_,
								   side,
								   price,
								   static_cast<quantity_t>(qty - booked));
			++unledgered_fills_;
		}
	}

	/// @brief Release whatever @p record's order still had working, if the
	///        record finishes it without an execution. @see
	///        risk_gate::on_outcome
	void retire(const engine::order_outcome &record) noexcept {
		const bool terminal = record.type == engine::OutcomeType::REJECTED ||
							  record.type == engine::OutcomeType::CANCELLED;
		if (!terminal) return;
		if (const auto retired = ledger_->retire(record.id))
			positions_->remove_working(symbol_, retired->side, retired->taken);
	}

	/// @brief Charge @p fee against the P&L. A 30-minute Demo Mode run
	///        without it reported -0.05 USDT while the account lost 3.02.
	void charge(notional_t fee) noexcept { fees_ += fee; }

	/// @brief Realised plus unrealised profit at @p mark, net of fees.
	[[nodiscard]] notional_t pnl(price_t mark) const noexcept {
		return positions_->snapshot(symbol_).pnl(mark) - fees_;
	}

	/// @brief Whether engine-printed executions move the position.
	void set_books_engine_fills(bool is_booking) noexcept {
		is_booking_engine_fills_ = is_booking;
	}

	/// @brief @see set_books_engine_fills
	[[nodiscard]] bool is_booking_engine_fills() const noexcept {
		return is_booking_engine_fills_;
	}

	/// @brief Commission charged so far, in tick-lots.
	[[nodiscard]] notional_t fees() const noexcept { return fees_; }

	/// @brief Engine prints of ours not booked because a venue is the market.
	[[nodiscard]] std::uint64_t ignored_engine_fills() const noexcept {
		return ignored_engine_fills_;
	}

	/// @brief Venue fills booked, in whole or part, after the ledger let go.
	[[nodiscard]] std::uint64_t unledgered_fills() const noexcept {
		return unledgered_fills_;
	}

private:
	void apply_side(order_id_t id, const engine::trade &execution) noexcept {
		const auto taken = ledger_->take(id, execution.volume);
		if (!taken) return;
		positions_->apply_fill(symbol_,
							   taken->side,
							   execution.price,
							   taken->taken);
		positions_->remove_working(symbol_, taken->side, taken->taken);
	}

	position_book *positions_;
	working_ledger *ledger_;
	symbol_id_t symbol_;
	notional_t fees_{};
	std::uint64_t unledgered_fills_     = 0;
	std::uint64_t ignored_engine_fills_ = 0;
	bool is_booking_engine_fills_       = true;
};

} // namespace exchange::risk::hooks::pre_trade
