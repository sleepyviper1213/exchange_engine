#pragma once
#include "core/util/enum_format.hpp"
#include "core/util/flag.hpp"

#include <cstdint>
#include <string_view>

namespace exchange::engine::event::lifecycle {

using core::util::format_as;

/// @brief The two places rebuilt state can come from, one bit each. Prints as
///        its enumerator name, e.g. @c "SNAPSHOT".
enum class recovery_mode : std::uint8_t {
	SNAPSHOT = 1U << 0U,
	JOURNAL  = 1U << 1U,
};

EXCHANGE_ENABLE_FLAGS(recovery_mode)

/// @brief What each source contributed, for logs.
[[nodiscard]] constexpr std::string_view
describe(recovery_mode value) noexcept {
	switch (value) {
		using enum recovery_mode;
	case SNAPSHOT: return "a checkpoint was loaded";
	case JOURNAL: return "journal entries were re-applied";
	}
	return {};
}

/**
 * @brief Where a rebuild's state came from - one source, or both.
 *
 * The three combinations that mean anything, and how to read them:
 *
 * | set | what happened |
 * | --- | ------------- |
 * | @c SNAPSHOT | a checkpoint was loaded and nothing had happened since |
 * | @c JOURNAL | replayed from an empty book; no snapshot was available |
 * | @c SNAPSHOT|JOURNAL | a checkpoint, then the entries after it |
 *
 * The one worth alerting on is a bare @c JOURNAL. Replaying from nothing is
 * correct but costs the whole history, and a venue doing it has lost its
 * checkpoints - the recovery succeeded and the operator still needs to know.
 * The other two differ only in whether anything happened after the last
 * checkpoint, which for a venue stopped outside trading hours is legitimately
 * nothing.
 */
using recovery_modes = core::util::flag<recovery_mode>;

} // namespace exchange::engine::event::lifecycle

template <>
struct magic_enum::customize::enum_range<
	exchange::engine::event::lifecycle::recovery_mode> {
	static constexpr bool is_flags = true; // NOLINT(*-identifier-naming)
};
