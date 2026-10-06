#pragma once
// Reading what is working at the venue, and withdrawing what is ours.
//
// Shared by `account`, where an operator asks, and by `serve`'s exit, where the
// process asks on its own behalf: after withdrawing the quotes it believes it
// has, it reads the venue's list and cancels whatever of ours is still there.
// That second read exists because the engine's belief has been wrong before -
// a testnet run exited "CLEAN" with two quotes still working, forgotten after
// an amendment the venue never received.

#include "session/venue_bridge.hpp"
#include "venue/credentials.hpp"
#include "venue/environment.hpp"
#include "venue/weight_budget.hpp"

#include <optional>
#include <string>
#include <vector>

namespace exchange::app {

/// @brief Where to ask, and as whom.
struct venue_access {
	std::string symbol{};
	venue::credentials credential{};
	venue::environment env = venue::environment::testnet;
	bool insecure_tls      = false;
};

/// @brief The client ids of every order working on @p access's listing, or
///        nothing if the venue could not be asked - the reason is logged.
[[nodiscard]] std::optional<std::vector<std::string>>
read_open_orders(const venue_access &access, venue::weight_budget &budget);

/// @brief One line per open order, with what reconciling it concluded.
void report_open(const std::vector<session::reconciled_order> &found);

/**
 * @brief Withdraw every working order this engine placed.
 *
 * @return Whether every one of ours is now gone.
 *
 * @par Why one request per order rather than the venue's bulk cancel
 * Binance has @c DELETE @c /api/v3/openOrders, which withdraws every order on a
 * symbol in a single call of one weight. It is not usable here, and the reason
 * is the whole point of the client-order-id prefix: that endpoint cancels
 * orders this process did not place, including ones a human placed by hand in
 * the venue's web UI while this was running. Paying one weight per order buys
 * the ability to leave those alone.
 */
[[nodiscard]] bool
cancel_ours(const std::vector<session::reconciled_order> &found,
			const venue_access &access, venue::weight_budget &budget);

} // namespace exchange::app
