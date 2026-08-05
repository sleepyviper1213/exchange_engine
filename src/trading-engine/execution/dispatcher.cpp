#include "dispatcher.hpp"


#include <bit>

namespace exchange::engine::execution {
dispatcher::dispatcher(std::size_t partition_count) noexcept
	: count_(partition_count),
	  // A power-of-two count turns the modulo into a mask. Precomputed
	  // rather than branched on per call: the test is on a member fixed for
	  // the dispatcher's life, so the branch below predicts perfectly, and
	  // the division it avoids is 20-40 cycles on a path that runs once per
	  // command.
	  mask_(std::has_single_bit(partition_count) ? partition_count - 1 : 0),
	  power_of_two_(std::has_single_bit(partition_count)) {
	assert(partition_count > 0 && "a dispatcher needs at least one partition");
}

[[nodiscard]] std::size_t
dispatcher::partition_for(symbol_id_t symbol) const noexcept {
	const auto key = static_cast<std::size_t>(symbol);
	return power_of_two_ ? (key & mask_) : (key % count_);
}

[[nodiscard]] std::size_t
dispatcher::partition_for(const event::command &cmd) const noexcept {
	return partition_for(cmd.symbol);
}

[[nodiscard]] std::size_t dispatcher::partition_count() const noexcept {
	return count_;
}

[[nodiscard]] bool dispatcher::owns(std::size_t partition,
									symbol_id_t symbol) const noexcept {
	return partition_for(symbol) == partition;
}


} // namespace exchange::engine::execution