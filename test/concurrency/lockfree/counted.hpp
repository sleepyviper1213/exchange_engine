#pragma once
#include<atomic>

/// @brief Instance-counting element; @c alive is atomic because the producer
/// constructs and the consumer destroys concurrently.
/// @note Copyable as well as movable: @c try_emplace_range copy-constructs from
/// its source range, so a move-only element cannot reach the batch push path.
struct counted {
	static inline std::atomic<int> alive{0};
	int value = 0;

	/// Non-explicit, so an array of these can be value-initialised as a batch
	/// buffer; the widening int conversion stays explicit.
	counted() noexcept { alive.fetch_add(1); }

	explicit counted(int v) noexcept : value(v) { alive.fetch_add(1); }

	counted(const counted &o) noexcept : value(o.value) { alive.fetch_add(1); }

	counted(counted &&o) noexcept : value(o.value) { alive.fetch_add(1); }

	counted &operator=(const counted &o) noexcept {
		value = o.value;
		return *this;
	}

	counted &operator=(counted &&o) noexcept {
		value = o.value;
		return *this;
	}

	~counted() { alive.fetch_sub(1); }
};
