#include "probe_table.hpp"

#include <bit>
#include <cstdint>

namespace exchange::risk::detail {

probe_table::probe_table(std::size_t slot_count)
	: slots_(slot_count),
	  mask_(slot_count - 1),
	  shift_(static_cast<unsigned>(64 - std::countr_zero(slot_count))) {}

[[nodiscard]] std::size_t probe_table::slot_count() const noexcept {
	return slots_.size();
}

[[nodiscard]] const ledger_slot &
probe_table::operator[](std::size_t at) const noexcept {
	return slots_[at];
}

[[nodiscard]] ledger_slot &probe_table::operator[](std::size_t at) noexcept {
	return slots_[at];
}

[[nodiscard]] std::size_t probe_table::find(order_id_t id) const noexcept {
	std::size_t at = home(id);
	while (slots_[at].id != 0) {
		if (slots_[at].id == id) return at;
		at = (at + 1) & mask_;
	}
	return NOT_FOUND;
}

[[nodiscard]] std::size_t
probe_table::vacancy_for(order_id_t id) const noexcept {
	std::size_t at = home(id);
	while (slots_[at].id != 0) {
		if (slots_[at].id == id) return NOT_FOUND;
		at = (at + 1) & mask_;
	}
	return at;
}

void probe_table::erase(std::size_t at) noexcept {
	std::size_t hole = at;
	for (;;) {
		slots_[hole]      = {};
		std::size_t probe = hole;
		for (;;) {
			probe = (probe + 1) & mask_;
			if (slots_[probe].id == 0) return;
			const std::size_t ideal = home(slots_[probe].id);
			// Is `ideal` cyclically inside (hole, probe]? If so this entry
			// is already found by a probe starting at its home and must not
			// move; if not, moving it into the hole keeps its chain intact.
			const bool must_stay = hole <= probe
									   ? (hole < ideal && ideal <= probe)
									   : (hole < ideal || ideal <= probe);
			if (!must_stay) break;
		}
		slots_[hole] = slots_[probe];
		hole         = probe;
	}
}

void probe_table::clear() noexcept {
	for (ledger_slot &s : slots_) s = {};
}

[[nodiscard]] std::size_t probe_table::home(order_id_t id) const noexcept {
	constexpr std::uint64_t GOLDEN = 0x9E37'79B9'7F4A'7C15ULL;
	return static_cast<std::size_t>((id * GOLDEN) >> shift_);
}


} // namespace exchange::risk::detail