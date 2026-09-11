#pragma once
// The account check: prove this process can authenticate, and say what the
// venue thinks is working.
//
// Deliberately read-only. Everything the order path needs is now built and
// tested offline - signing, encoding, the weight budget, reconciliation - and
// the one thing no test can establish is whether a signature this process
// produces is one the *venue* accepts. That is what this command answers, and
// it answers it without placing anything.
//
// It is also the operational read: `GET /api/v3/openOrders` is what settles the
// question an unanswered placement leaves open, and `venue_bridge`'s
// reconciliation is what turns the answer into something to act on. Running it
// before wiring order flow is the point; running it *during* an incident, to
// see what is actually working on the account, is the reason it stays.

#include "venue/credentials.hpp"
#include "venue/environment.hpp"

#include <string>

namespace exchange::app {

/// @brief What `trade` was asked to do.
struct account_settings {
	/// @brief The listing to ask about, as the venue spells it.
	std::string symbol = "SOLUSDT";

	/**
	 * @brief Which deployment to talk to.
	 *
	 * Testnet unless the operator typed @c --live. The default is the whole
	 * safety story: a command that authenticates against a real account should
	 * take an explicit act to point at the one with money in it.
	 */
	venue::environment env = venue::environment::testnet;

	/// @brief API key and secret, from the environment and nowhere else.
	/// @see app/credentials_option.hpp
	venue::credentials credential{};

	/// @brief Accept any TLS certificate. For a host with no CA bundle - and
	///        never for a credentialed request unless you know the network.
	bool insecure_tls = false;

	/**
	 * @brief Withdraw every working order this engine placed, then report.
	 *
	 * The get-me-flat action, and the only destructive thing this command can
	 * do. It cancels one order at a time rather than using the venue's bulk
	 * endpoint, because the bulk one would take orders this process did not
	 * place with it. @see cmd_account
	 */
	bool cancel_all = false;
};

/**
 * @brief Authenticate against the venue and report what is working.
 *
 * @return @c EXIT_SUCCESS when the venue answered, whatever it said about open
 *         orders - zero of them is a perfectly good answer. @c EXIT_FAILURE
 *         when the credential is missing, the request could not be built, the
 *         venue refused it, or - under @c cancel_all - an order of ours is
 *         still working when this returns.
 *
 * @warning With @c cancel_all it withdraws orders. It withdraws only the ones
 *          whose client order id this process would have written; anything
 *          else on the account is reported and left alone.
 *          @see session::CLIENT_ORDER_PREFIX
 */
[[nodiscard]] int cmd_account(const account_settings &settings);

} // namespace exchange::app
