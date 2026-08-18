#pragma once
// `exchange_tool recover` - run a partition against a durable store, recovering
// whatever the last run left behind.
//
// The one command whose interesting behaviour is what happens the *second* time
// you run it. Everything else in this tool starts from nothing; this starts from
// the journal and snapshot in a directory, replays them, adds to them, and leaves
// them for the next run. Run it twice and the resting book from the first run is
// there at the start of the second - which is the whole of docs/recovery.md, in a
// binary rather than in a test.

#include <cstdint>
#include <string>

namespace exchange::app {

/// @brief Everything `recover` was asked for. @see add_recover
struct recover_settings {
	std::string store = "var/venue"; ///< directory holding journal + manifest
	std::uint64_t orders = 8;        ///< resting orders to add after recovering
	bool checkpoint      = false;    ///< snapshot + commit before shutting down
	bool recover_only    = false;    ///< recover and report, adding nothing
};

/**
 * @brief Recover the store, optionally add flow, optionally checkpoint.
 *
 * @return @c EXIT_SUCCESS, or @c EXIT_FAILURE with the reason logged - including
 *         when the journal could not be made durable, which is not a degraded
 *         mode to carry on in.
 */
int cmd_recover(const recover_settings &settings);

} // namespace exchange::app
