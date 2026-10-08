#pragma once
// The kill switch's signal: a file `serve` keeps touching while it is alive.
//
// Binance spot has no cancel-on-disconnect - not on REST, not on the WebSocket
// API, not in the FIX Logon - so a `serve` that dies leaves its quotes resting
// with nothing managing them. Its own exit sweep cannot help with that: a
// process that crashed, was killed, or lost its machine runs no exit path at
// all. One did, on 2026-10-06, killed mid-run with orders still able to fill.
//
// So the switch has to live outside the process: `exchange_tool watchdog`
// reads this file and, when a running `serve` stops touching it, withdraws
// every order carrying our client-id prefix.
//
// --- why the file's age and not its contents
// ----------------------------------
//
// Liveness is the modification time, which the filesystem updates atomically
// with every write; the contents only say *which* state the writer was in. A
// reader that catches a write half-done still sees a fresh time, so a torn read
// can never make a live process look dead, nor a dead one live.
//
// --- why "stopped" is written last
// --------------------------------------------
//
// After the exit sweep, not before. A process that dies anywhere in its own
// shutdown is then still `running` with a stale time, and the watchdog acts -
// which is the case the switch exists for.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string_view>
#include <system_error>

namespace exchange::app {

/// @brief What the writer said it was doing.
enum class heartbeat_state : std::uint8_t {
	running, ///< quoting, or able to; its orders need a live process
	stopped, ///< shut down cleanly, exit sweep done; nothing to protect
};

/// @brief The watchdog's reading of one heartbeat.
enum class watchdog_verdict : std::uint8_t {
	absent,  ///< no file yet - nothing has started, so nothing to protect
	alive,   ///< touched recently
	stopped, ///< the writer finished cleanly
	stale,   ///< the writer stopped touching it without saying so: act
};

/// @brief One heartbeat, as read.
struct heartbeat_reading {
	/// @brief The state the file names, or nothing if it named none - a torn
	///        or foreign file.
	std::optional<heartbeat_state> state{};

	/// @brief Time since the file was last written.
	std::chrono::milliseconds age{};
};

/// @brief The line a heartbeat file holds for @p state.
[[nodiscard]] constexpr std::string_view
heartbeat_text(heartbeat_state state) noexcept {
	return state == heartbeat_state::stopped ? "stopped\n" : "running\n";
}

/// @brief The state @p text names, or nothing.
[[nodiscard]] constexpr std::optional<heartbeat_state>
parse_heartbeat(std::string_view text) noexcept {
	if (text.starts_with("running")) return heartbeat_state::running;
	if (text.starts_with("stopped")) return heartbeat_state::stopped;
	return std::nullopt;
}

/**
 * @brief Whether @p reading means the writer died with orders at risk.
 *
 * @param reading The file's state and age, or nothing if there is no file.
 * @param stale_after How long without a write is death rather than a pause.
 *
 * @note Unreadable *and* stale is read as stale: a writer that left garbage
 *       and then went quiet is the one case where acting on too little is
 *       better than waiting. Unreadable but fresh is alive - a torn read of a
 *       file somebody is still writing.
 */
[[nodiscard]] constexpr watchdog_verdict
judge(const std::optional<heartbeat_reading> &reading,
	  std::chrono::milliseconds stale_after) noexcept {
	if (!reading.has_value()) return watchdog_verdict::absent;
	if (reading->state == heartbeat_state::stopped)
		return watchdog_verdict::stopped;
	return reading->age > stale_after ? watchdog_verdict::stale
									  : watchdog_verdict::alive;
}

/// @brief Overwrite @p path with @p state. @return Whether it was written.
[[nodiscard]] inline bool write_heartbeat(const std::filesystem::path &path,
										  heartbeat_state state) {
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out << heartbeat_text(state);
	out.flush();
	return static_cast<bool>(out);
}

/// @brief Read @p path, or nothing if it does not exist or cannot be read.
[[nodiscard]] inline std::optional<heartbeat_reading>
read_heartbeat(const std::filesystem::path &path) {
	std::error_code ec;
	const auto written = std::filesystem::last_write_time(path, ec);
	if (ec) return std::nullopt;

	char text[16]{};
	std::ifstream in(path, std::ios::binary);
	in.read(text, sizeof text - 1);
	const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::filesystem::file_time_type::clock::now() - written);
	return heartbeat_reading{
		.state = parse_heartbeat(
			std::string_view(text, static_cast<std::size_t>(in.gcount()))),
		.age = age};
}

} // namespace exchange::app
