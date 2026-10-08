#include "watchdog.hpp"

#include "app/commands/venue_orders.hpp"
#include "app/credentials_option.hpp"
#include "app/heartbeat.hpp"
#include "session/venue_bridge.hpp"
#include "venue/weight_budget.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdlib>
#include <thread>

namespace exchange::app {
namespace {

/// Withdraw every order of ours on the watched listing. @return Whether none
/// is left working.
[[nodiscard]] bool withdraw_ours(const venue_access &access) {
	venue::weight_budget budget;
	const auto open = read_open_orders(access, budget);
	if (!open) return false;
	const auto found = session::reconcile({}, *open);
	report_open(found);
	return cancel_ours(found, access, budget);
}

} // namespace

int cmd_watchdog(const watchdog_settings &settings) {
	spdlog::info("{}", describe(settings.credential));
	if (!settings.credential.is_complete()) {
		spdlog::error("set {} and {} in the environment - the watchdog needs "
					  "the same credential as the serve it watches",
					  venue::API_KEY_VAR,
					  venue::API_SECRET_VAR);
		return EXIT_FAILURE;
	}
	const venue_access access{.symbol     = settings.symbol,
							  .credential = settings.credential,
							  .env        = settings.env};
	const std::chrono::milliseconds stale_after{settings.stale_ms};
	spdlog::info("watching {} for {} on {}: withdrawing our orders after {} ms "
				 "without a heartbeat",
				 settings.heartbeat_file.string(),
				 settings.symbol,
				 to_string(settings.env),
				 settings.stale_ms);

	// Armed while the writer is alive; fired once per outage.
	bool is_fired         = false;
	watchdog_verdict last = watchdog_verdict::absent;
	for (;;) {
		const watchdog_verdict now =
			judge(read_heartbeat(settings.heartbeat_file), stale_after);
		if (now != last && now == watchdog_verdict::alive)
			spdlog::info("heartbeat is live");
		last = now;

		switch (now) {
		case watchdog_verdict::absent: break;
		case watchdog_verdict::alive: is_fired = false; break;
		case watchdog_verdict::stopped:
			spdlog::info("serve stopped cleanly; nothing to protect");
			return EXIT_SUCCESS;
		case watchdog_verdict::stale:
			if (is_fired) break;
			spdlog::warn("no heartbeat for over {} ms - serve is gone or hung; "
						 "withdrawing every order of ours on {}",
						 settings.stale_ms,
						 settings.symbol);
			// Fired only once it worked: a venue that could not be reached is
			// asked again on the next poll rather than given up on.
			is_fired = withdraw_ours(access);
			if (is_fired)
				spdlog::warn("withdrawn; waiting for serve to return");
			else spdlog::error("the withdrawal did not complete; retrying");
			break;
		}
		std::this_thread::sleep_for(
			std::chrono::milliseconds{settings.poll_ms});
	}
}

} // namespace exchange::app
