#include "core/logging/channels.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <cstddef>
#include <memory>
#include <string>

namespace exchange::core::logging {
namespace {

/// @brief The wire name each channel prints under, in enumerator order.
constexpr std::array<const char *, 4> CHANNEL_NAMES{
	"matching",
	"risk",
	"trading",
	"feed",
};

[[nodiscard]] constexpr std::size_t index_of(channel which) noexcept {
	return static_cast<std::size_t>(which);
}

/**
 * @brief The installed channels, or empty until one is asked for.
 *
 * Function-local so there is no ordering question at start-up, and
 * @c shared_ptr so the sinks a channel writes to cannot be destroyed while a
 * channel still points at them - which is the failure @c lifecycle.hpp's
 * static-destructor warning describes from the other side.
 */
[[nodiscard]] std::array<std::shared_ptr<spdlog::logger>,
						 CHANNEL_NAMES.size()> &
slots() noexcept {
	static std::array<std::shared_ptr<spdlog::logger>, CHANNEL_NAMES.size()>
		installed;
	return installed;
}

/// @brief A clone of the current default logger under @p name - same sinks,
///        same level, same pattern - registered so `spdlog::get` finds it too.
[[nodiscard]] std::shared_ptr<spdlog::logger> clone_default(const char *name) {
	auto cloned = spdlog::default_logger()->clone(name);
	// Dropped first so re-installing over an earlier default (a second
	// `init`, or a test replacing the logger) is not an
	// already-registered error.
	spdlog::drop(name);
	try {
		spdlog::register_logger(cloned);
	} catch (const spdlog::spdlog_ex &) {
		// NOLINT(bugprone-empty-catch) - registration is a convenience for
		// `spdlog::get`, and the reference this file hands out does not depend
		// on it. A logger that could not be registered is not worth failing a
		// process over, and there is nothing to report it *with*: the reporting
		// channel is the thing that just failed.
	}
	return cloned;
}

} // namespace

void install_channels() {
	auto &installed = slots();
	for (std::size_t i = 0; i < CHANNEL_NAMES.size(); ++i)
		installed[i] = clone_default(CHANNEL_NAMES[i]);
}

spdlog::logger &logger_for(channel which) noexcept {
	auto &installed      = slots();
	const std::size_t at = index_of(which);
	// A `channel` cast from an out-of-range integer would index past both
	// arrays. Guarded rather than asserted because this is the function
	// everything else logs *through* - the default logger is a correct answer
	// to "which channel is that", where a terminating assert in the logging
	// path is not.
	if (at >= CHANNEL_NAMES.size()) return *spdlog::default_logger();
	// Lazily, and only on the first miss: a process that never called `init`
	// still gets a working channel over spdlog's own default sinks rather than
	// a null dereference. `init` overwrites these, so the common path is one
	// pointer load.
	if (installed[at] == nullptr) {
		try {
			installed[at] = clone_default(CHANNEL_NAMES[at]);
		} catch (...) {
			// Nothing left to do but log through the default logger, which is
			// what returning it amounts to. A channel that cannot be cloned is
			// a broken logging setup, not a reason to stop trading.
			return *spdlog::default_logger();
		}
	}
	return *installed[at];
}

} // namespace exchange::core::logging
