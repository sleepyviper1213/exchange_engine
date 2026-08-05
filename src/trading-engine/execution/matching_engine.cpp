#include "matching_engine.hpp"

namespace exchange::engine::execution {
namespace {

/// The reserved anonymous id, matching order_book's own sentinel: such an order
/// rests and matches but is not indexed and produces no outcomes, so there is
/// nobody a rejection could be reported to either.
constexpr order_id_t ANONYMOUS = 0;

} // namespace

matching_engine::matching_engine(book_manager &books) noexcept
	: books_(&books) {}

bool matching_engine::process(const command &cmd, std::vector<Trade> &trades,
							  std::vector<OrderOutcome> &outcomes) {
	order_book *book = books_->lookup(cmd.symbol);
	if (book == nullptr) [[unlikely]] {
		reject_misrouted(cmd, outcomes);
		return false;
	}

	switch (cmd.type) {
	case command::Type::PLACE:
		book->place_order(cmd.order_, trades, outcomes);
		break;
	case command::Type::CANCEL:
		book->cancel_order(cmd.cancel_id, outcomes);
		break;
	case command::Type::ADD:
		book->add_order(cmd.level.side, cmd.level.price, cmd.level.volume);
		break;
	case command::Type::REDUCE:
		book->delete_order(cmd.level.side, cmd.level.price, cmd.level.volume);
		break;
	}
	return true;
}

void matching_engine::reject_misrouted(const command &cmd,
									   std::vector<OrderOutcome> &outcomes) {
	switch (cmd.type) {
	case command::Type::PLACE:
		// Anonymous liquidity has no client to answer, exactly as inside the
		// book — an id of 0 is never reported on.
		if (cmd.order_.id != ANONYMOUS)
			outcomes.push_back(
				OrderOutcome::rejected(cmd.order_.id,
									   reject_reason::UNKNOWN_SYMBOL,
									   cmd.order_.qty));
		break;
	case command::Type::CANCEL:
		// UNKNOWN_SYMBOL rather than UNKNOWN_ORDER: the order may well exist,
		// on the partition this cancel should have reached. Reporting the order
		// as unknown would send the client looking in the wrong place.
		outcomes.push_back(
			OrderOutcome::cancel_rejected(cmd.cancel_id,
										  reject_reason::UNKNOWN_SYMBOL));
		break;
	case command::Type::ADD:
	case command::Type::REDUCE:
		// Depth carries no identity, so a misroute here is observable only in
		// the partition's misrouted() counter.
		break;
	}
}

book_manager &matching_engine::books() const noexcept { return *books_; }

} // namespace exchange::engine::execution
