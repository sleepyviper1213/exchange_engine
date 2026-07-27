#pragma once
#include "core_export.hpp"

#include <atomic>

/// @brief Instance-counting element; @c alive is atomic because the producer
/// constructs and the consumer destroys concurrently.
/// @note Copyable as well as movable: @c try_emplace_range copy-constructs from
/// its source range, so a move-only element cannot reach the batch push path.
namespace exchange::core::util{
struct CORE_AUTOTEST_EXPORT counted {
	static inline std::atomic<int> alive{0};
	int value = 0; // NOLINT(misc-non-private-member-variables-in-classes)

	/// Non-explicit, so an array of these can be value-initialised as a
	/// batch buffer; the widening int conversion stays explicit.
	counted() noexcept;

	explicit counted(int v) noexcept;

	counted(const counted &o) noexcept;

	counted(counted &&o) noexcept;

	counted &operator=(const counted &o) noexcept = default;

	counted &operator=(counted &&o) noexcept = default;

	~counted();
};
} // namespace exchange::util
