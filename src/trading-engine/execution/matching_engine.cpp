#include "matching_engine.hpp"

#include <cassert>

namespace exchange::engine::execution {
namespace {

/// The reserved anonymous id, matching order_book's own sentinel: such an order
/// rests and matches but is not indexed and produces no outcomes, so there is
/// nobody a rejection could be reported to either.
constexpr order_id_t ANONYMOUS = 0;

} // namespace

matching_engine::matching_engine(book_manager &books,
								 order_manager &orders) noexcept
	: books_(&books), orders_(&orders) {}

bool matching_engine::process(const command &cmd, std::vector<trade> &trades,
							  std::vector<order_outcome> &outcomes) {
	order_book *book = books_->lookup(cmd.symbol);
	if (book == nullptr) [[unlikely]] {
		reject_misrouted(cmd, outcomes);
		return false;
	}

	switch (cmd.type) {
	case command::Type::PLACE:
		place(*book, cmd.as_place(), trades, outcomes);
		break;
	case command::Type::CANCEL:
		cancel(*book, cmd.as_cancel(), outcomes);
		break;
	case command::Type::ADD: {
		const auto &lvl = cmd.as_level();
		book->add_order(lvl.side, lvl.price, lvl.volume);
		break;
	}
	case command::Type::REDUCE: {
		const auto &lvl = cmd.as_level();
		book->delete_order(lvl.side, lvl.price, lvl.volume);
		break;
	}
	}
	return true;
}

void matching_engine::place(order_book &book, const orders::order &incoming,
							std::vector<trade> &trades,
							std::vector<order_outcome> &outcomes) {
	// Anonymous liquidity belongs to nobody, so there is no record to keep and
	// nobody to report to. It goes straight to the book, exactly as before.
	if (incoming.id == ANONYMOUS) {
		book.place_order(incoming, trades, outcomes);
		return;
	}

	// Admission first, and the book untouched if it fails. The manager's refusals
	// are the ones the book cannot make — chiefly an id that is spent because an
	// earlier order under it *finished*, which the book has already forgotten.
	const auto admitted = orders_->admit(incoming);
	if (!admitted.has_value()) [[unlikely]] {
		outcomes.push_back(order_outcome::rejected(incoming.id, admitted.error(),
												  incoming.qty));
		return;
	}

	const std::size_t first = outcomes.size();
	book.place_order(incoming, trades, outcomes);
	reconcile(outcomes, first);
}

void matching_engine::cancel(order_book &book, order_id_t id,
							 std::vector<order_outcome> &outcomes) {
	const std::size_t first = outcomes.size();
	book.cancel_order(id, outcomes);

	for (std::size_t i = first; i < outcomes.size(); ++i) {
		order_outcome &answer = outcomes[i];
		if (answer.type != OutcomeType::CANCEL_REJECTED ||
			answer.reason != reject_reason::UNKNOWN_ORDER)
			continue;

		// The book said "no such resting order", which is the truth it has: its
		// index holds resting orders only. The manager kept the record, so it can
		// say which kind of "no" this is.
		//
		// NONE means the manager still thinks the order is live while the book
		// has no such order resting, and there is exactly one way that happens: a
		// REDUCE drained the level it sat on. Depth commands carry no identity and
		// emit no outcomes, so nothing tells the manager an identified order went
		// with one. Not an assertion, because the sequence is legal today — the
		// book's UNKNOWN_ORDER stands, which is the more conservative of the two
		// answers, and the record is left alone rather than guessed at.
		if (const reject_reason remembered = orders_->cancellable(id);
			remembered != reject_reason::NONE)
			answer.reason = remembered;
	}

	reconcile(outcomes, first);
}

void matching_engine::reconcile(const std::vector<order_outcome> &outcomes,
								std::size_t first) {
	for (std::size_t i = first; i < outcomes.size(); ++i) {
		const order_outcome &event = outcomes[i];
		const order_handle handle = orders_->find(event.id);
		const order_record *record = orders_->get(handle);
		// No record: an anonymous resting order the book filled, or one whose
		// history has aged out. Neither is an error — there is simply nothing to
		// bring up to date.
		if (record == nullptr) continue;

		switch (event.type) {
		case OutcomeType::FILL:
			// The outcome carries the order's cumulative traded quantity, not the
			// increment, so the increment is the difference. Taking it this way
			// means a record can never drift from what the client was told: it is
			// driven *to* the reported total rather than nudged alongside it.
			if (const quantity_t executed = event.traded - record->state.traded();
				executed > 0)
				orders_->apply_fill(handle, executed);
			break;
		case OutcomeType::CANCELLED:
			// A dropped IOC remainder or a client cancel the book applied. Guarded
			// because a fill in the same batch may already have finished the order,
			// and a terminal record does not change again.
			if (record->is_active()) orders_->cancel(handle, event.reason);
			break;
		case OutcomeType::REJECTED:
			// The book refused an order the manager admitted — an unsupported
			// type, or a fill-or-kill the liquidity could not cover.
			if (record->is_active()) orders_->reject(handle, event.reason);
			break;
		case OutcomeType::ACCEPTED:
		case OutcomeType::CANCEL_REJECTED:
			// Neither moves a record. ACCEPTED restates what admit() already
			// wrote, and a declined cancel leaves its target exactly as it was —
			// which is the whole point of declining it.
			break;
		}
	}
}

void matching_engine::reject_misrouted(const command &cmd,
									   std::vector<order_outcome> &outcomes) {
	switch (cmd.type) {
	case command::Type::PLACE: {
		// Anonymous liquidity has no client to answer, exactly as inside the
		// book — an id of 0 is never reported on.
		const auto &placed = cmd.as_place();
		if (placed.id != ANONYMOUS)
			outcomes.push_back(
				order_outcome::rejected(placed.id, reject_reason::UNKNOWN_SYMBOL,
									   placed.qty));
		break;
	}
	case command::Type::CANCEL:
		// UNKNOWN_SYMBOL rather than UNKNOWN_ORDER: the order may well exist,
		// on the partition this cancel should have reached. Reporting the order
		// as unknown would send the client looking in the wrong place.
		outcomes.push_back(
			order_outcome::cancel_rejected(cmd.as_cancel(),
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

order_manager &matching_engine::orders() const noexcept { return *orders_; }

} // namespace exchange::engine::execution
