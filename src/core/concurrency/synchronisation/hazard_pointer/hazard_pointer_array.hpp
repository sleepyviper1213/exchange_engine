#pragma once
#include "hazard_pointer.hpp"

#include <array>

namespace exchange::core::concurrency::synchronisation {

// A fixed-size bundle of N hazard pointers acquired together — convenient
// for algorithms (list/tree traversal) that must protect several pointers
// at once.
template <std::size_t N>
class hazard_pointer_array {
public:
	hazard_pointer_array() {
		for (auto &h : haz_) h = make_hazard_pointer();
	}

	explicit hazard_pointer_array(hazard_pointer_domain &domain) {
		for (auto &h : haz_) h = make_hazard_pointer(domain);
	}

	hazard_pointer &operator[](std::size_t i) noexcept { return haz_[i]; }

	const hazard_pointer &operator[](std::size_t i) const noexcept {
		return haz_[i];
	}

	static constexpr std::size_t size() noexcept { return N; }

private:
	std::array<hazard_pointer, N> haz_{};
};
} // namespace exchange::core::concurrency::synchronisation
