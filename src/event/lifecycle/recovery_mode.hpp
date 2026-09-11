#pragma once
#include "core/util/enum_string.hpp"
#include "core/util/flag.hpp"

#include <cstdint>

namespace exchange::engine::event::lifecycle {
/// @brief The two places rebuilt state can come from, one bit each.
#define RECOVERY_MODE_LIST(X)                                                  \
	X(SNAPSHOT, 1U << 0U, "a checkpoint was loaded")                           \
	X(JOURNAL, 1U << 1U, "journal entries were re-applied")

enum class recovery_mode : std::uint8_t {
	EXCHANGE_ENUM_VALUED_VALUES(RECOVERY_MODE_LIST)
};

EXCHANGE_ENABLE_FLAGS(recovery_mode)

/// @brief The enumerator name of @p value, e.g. @c "SNAPSHOT".
EXCHANGE_ENUM_VALUED_NAME(recovery_mode, to_string, RECOVERY_MODE_LIST)

/// @brief What each source contributed, for logs.
EXCHANGE_ENUM_VALUED_LABEL_ONLY(recovery_mode, describe, RECOVERY_MODE_LIST)

#undef RECOVERY_MODE_LIST
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