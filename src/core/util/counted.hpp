#pragma once
#include "core_export.hpp"

#include <atomic>

/// @brief Instance-counting element; @c alive is atomic because the producer
/// constructs and the consumer destroys concurrently.
/// @note Copyable as well as movable: @c try_emplace_range copy-constructs from
/// its source range, so a move-only element cannot reach the batch push path.
namespace exchange::core::util{
struct counted {
	CORE_AUTOTEST_EXPORT static std::atomic<int> alive;
	int value = 0; // NOLINT(misc-non-private-member-variables-in-classes)

	/// Non-explicit, so an array of these can be value-initialised as a
	/// batch buffer; the widening int conversion stays explicit.
	CORE_AUTOTEST_EXPORT counted() noexcept;

	CORE_AUTOTEST_EXPORT explicit counted(int v) noexcept;

	CORE_AUTOTEST_EXPORT counted(const counted &o) noexcept;

	CORE_AUTOTEST_EXPORT counted(counted &&o) noexcept;

	counted &operator=(const counted &o) noexcept = default;

	counted &operator=(counted &&o) noexcept = default;

	CORE_AUTOTEST_EXPORT ~counted();
};
} // namespace exchange::util
