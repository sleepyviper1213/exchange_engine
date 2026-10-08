#pragma once
// The kill switch: withdraw a dead `serve`'s orders. @see app/heartbeat.hpp

#include "venue/credentials.hpp"
#include "venue/environment.hpp"

#include <filesystem>
#include <string>

namespace exchange::app {

/// @brief What `watchdog` was asked to do.
struct watchdog_settings {
	/// @brief The file the watched `serve` was given as @c --heartbeat-file.
	std::filesystem::path heartbeat_file{};

	/// @brief The listing whose orders to withdraw, as the venue spells it.
	std::string symbol = "SOLUSDT";

	/// @brief Which deployment. Must match the watched `serve`'s.
	venue::environment env = venue::environment::testnet;

	/// @brief API key and secret, from the environment and nowhere else.
	venue::credentials credential{};

	/**
	 * @brief Silence, in milliseconds, after which the writer is taken to be
	 *        dead. Five heartbeats by default: long enough that a slow frame
	 *        or a busy disk is not death, short enough that a quote does not
	 *        rest unmanaged for long.
	 */
	int stale_ms = 5000;

	/// @brief How often to read the file, in milliseconds.
	int poll_ms = 500;
};

/**
 * @brief Watch @c heartbeat_file until its writer stops cleanly, withdrawing
 *        every order of ours whenever it dies instead.
 *
 * Fires once per outage, and arms again when the heartbeat comes back - a
 * restarted `serve` is watched like the first. A withdrawal that does not
 * complete is retried on the next poll.
 *
 * @return @c EXIT_SUCCESS once the writer reports a clean stop; it does not
 *         return otherwise. @c EXIT_FAILURE without a credential.
 *
 * @warning Withdraws only orders whose client id carries our prefix, the rule
 *          @c account @c --cancel-all keeps. @see session::CLIENT_ORDER_PREFIX
 */
[[nodiscard]] int cmd_watchdog(const watchdog_settings &settings);

} // namespace exchange::app
