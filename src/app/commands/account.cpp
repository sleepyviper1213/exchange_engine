#include "account.hpp"

#include "app/commands/venue_orders.hpp"
#include "app/credentials_option.hpp"
#include "session/venue_bridge.hpp"
#include "venue/weight_budget.hpp"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace exchange::app {

int cmd_account(const account_settings &settings) {
	spdlog::info("{}", describe(settings.credential));
	if (!settings.credential.is_complete()) {
		spdlog::error(
			"set {} and {} in the environment; they are read from there and "
			"from nowhere else",
			venue::API_KEY_VAR,
			venue::API_SECRET_VAR);
		return EXIT_FAILURE;
	}

	// Said every time, not only for production: an operator reading a log a
	// week later needs to know which account these orders were on - and the
	// environment is *named*, never described by a branch. A two-way message
	// over a three-way enum reported `demo` as "the testnet sandbox", which is
	// the one thing a log line about which account this is must not do.
	switch (settings.env) {
	case venue::environment::production:
		spdlog::warn("talking to PRODUCTION - these are real orders on a real "
					 "account");
		break;
	case venue::environment::testnet:
		spdlog::info("talking to {}: its own book, its own thin liquidity",
					 to_string(settings.env));
		break;
	case venue::environment::demo:
		spdlog::info("talking to {}: fake balances against depth that tracks "
					 "the live order_book",
					 to_string(settings.env));
		break;
	}

	const venue_access access{.symbol       = settings.symbol,
							  .credential   = settings.credential,
							  .env          = settings.env,
							  .insecure_tls = settings.insecure_tls};
	venue::weight_budget budget;

	const auto open = read_open_orders(access, budget);
	if (!open) return EXIT_FAILURE;
	spdlog::info("authenticated; {} order(s) working on {}",
				 open->size(),
				 settings.symbol);

	// Nothing is believed working locally: this process placed none of these,
	// it has just started. So every one of ours reads as `adopt`, which is the
	// honest finding - they are orders a previous run left behind.
	const auto found = session::reconcile({}, *open);
	report_open(found);

	bool ok = true;
	if (settings.cancel_all && !open->empty()) {
		spdlog::warn("withdrawing every order this engine placed; anything it "
					 "did not place is left alone");
		ok = cancel_ours(found, access, budget);
	}

	spdlog::info("rate limit: {} of {} weight left this minute",
				 budget.remaining(venue::weight_budget::clock::now()),
				 budget.limit());
	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace exchange::app
