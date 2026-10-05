#include "stop_handshake.hpp"

#include <thread>

namespace exchange::core::concurrency::synchronisation {

void stop_handshake::run(core::util::function_ref<size_t() const noexcept> turn,
						 core::util::function_ref<void() const noexcept> idle) {
	// Acquire pairs with the release in stop(). See the class note.
	while (!stopping_.load(std::memory_order_acquire))
		if (turn() == 0) idle();
	while (turn() != 0) {}
	// Release publishes everything the final drain wrote to the producer,
	// which reads it after its acquire of this flag.
	finished_.store(true, std::memory_order_release);
}

void stop_handshake::run(
	core::util::function_ref<size_t() const noexcept> turn) {
	run(turn, []() noexcept { std::this_thread::yield(); });
}

void stop_handshake::stop(
	core::util::function_ref<size_t() const noexcept> pump) {
	stopping_.store(true, std::memory_order_release);
	while (!finished_.load(std::memory_order_acquire))
		if (pump() == 0) std::this_thread::yield();
}

[[nodiscard]] bool stop_handshake::has_finished() const noexcept {
	return finished_.load(std::memory_order_acquire);
}
} // namespace exchange::core::concurrency::synchronisation