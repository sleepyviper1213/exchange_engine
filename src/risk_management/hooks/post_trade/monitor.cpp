#include "monitor.hpp"

namespace exchange::risk::hooks::post_trade {

post_trade_monitor::post_trade_monitor(system::circuit_breaker &breaker,
									   symbol_id_t symbol,
									   const post_trade_limits &limits,
									   std::uint64_t now_ns) noexcept
	: symbol_(symbol),
	  limits_(limits),
	  ratio_(breaker, limits),
	  fills_(breaker, limits),
	  silence_(breaker, limits, now_ns) {}

bool post_trade_monitor::on_trades(std::span<const engine::trade> executions,
								   std::uint64_t now_ns) noexcept {
	bool tripped = false;
	for (const engine::trade &print : executions) {
		// Both rules see every print, and the burst rule's result is ORed
		// rather than short-circuited: a trip stops nothing here. The counters
		// have to keep counting through an open breaker, or an operator
		// re-arming would be re-arming into numbers that stopped at the moment
		// they became interesting.
		tripped |= fills_.record(now_ns, print);
		ratio_.record_execution(now_ns);
	}
	return tripped;
}

bool post_trade_monitor::on_outcomes(
	std::span<const engine::order_outcome> records,
	std::uint64_t now_ns) noexcept {
	bool tripped = false;
	for (const engine::order_outcome &record : records) {
		// Every record is evidence the path is alive, whatever it says.
		silence_.beat(now_ns);
		if (is_venue_message(record)) tripped |= ratio_.record_message(now_ns);
	}
	return tripped;
}

bool post_trade_monitor::poll(std::uint64_t now_ns,
							  std::uint32_t working) noexcept {
	return silence_.poll(now_ns, working);
}

symbol_id_t post_trade_monitor::symbol() const noexcept { return symbol_; }

const post_trade_limits &post_trade_monitor::limits() const noexcept {
	return limits_;
}

const order_trade_ratio &post_trade_monitor::ratio() const noexcept {
	return ratio_;
}

const fill_burst &post_trade_monitor::fills() const noexcept { return fills_; }

const outcome_silence &post_trade_monitor::silence() const noexcept {
	return silence_;
}

std::uint64_t post_trade_monitor::trips() const noexcept {
	return ratio_.trips() + fills_.trips() + silence_.trips();
}

} // namespace exchange::risk::hooks::post_trade
