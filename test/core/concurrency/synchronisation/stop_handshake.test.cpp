#include "core/concurrency/synchronisation/stop_handshake.hpp"

#include "core/concurrency/lockfree/spsc_queue.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <thread>

// The protocol that ends a producer/consumer pair whose consumer publishes
// back. Two properties, one per flag, and each suite here is the failure the
// shutdown it replaced actually had.

using namespace exchange::core::concurrency::lockfree;
using namespace exchange::core::concurrency::synchronisation;

namespace {

// Every command submitted before stop() is applied by the final drain.
//
// What failure looks like: `applied` short of `submitted` on some round,
// because the consumer read the stop flag as set and then a stale write
// cursor, found the queue empty and returned. That needs the flag to be
// relaxed *and* a weakly ordered CPU, so on x86 this cannot fail whatever the
// ordering; it is here for the arm64 runners, where a relaxed flag admits it.
// The argument it is evidence for is in stop_handshake.hpp: release on the
// flag, acquire on its load, and read coherence on the cursor.
TEST(StopHandshake, TheFinalDrainAppliesEverythingSubmittedBeforeTheStop) {
	constexpr int HANDSHAKE_ROUNDS = 500;
	constexpr int HANDSHAKE_BURST  = 48;

	for (int round = 0; round < HANDSHAKE_ROUNDS; ++round) {
		spsc_queue<int, 64> commands;
		stop_handshake handshake;
		int applied = 0;

		std::thread consumer([&] {
			handshake.run([&]() noexcept {
				std::size_t n = 0;
				while (commands.try_dequeue()) ++n;
				applied += static_cast<int>(n);
				return n;
			});
		});

		for (int i = 0; i < HANDSHAKE_BURST; ++i)
			while (!commands.try_emplace(i)) std::this_thread::yield();
		handshake.stop([]() noexcept { return std::size_t{0}; });
		consumer.join();

		ASSERT_EQ(applied, HANDSHAKE_BURST) << "on round " << round;
	}
}

// The consumer's last turn publishes more than the return ring holds, and then
// waits for room - which only the producer can make.
//
// What failure looks like: a hang, not a red assertion. A producer that stops
// and then joins without pumping leaves both threads waiting on each other
// forever, so the regression this guards against shows up as the test
// timing out.
TEST(StopHandshake, StopPumpsTheReturnPathUntilTheConsumerHasFinished) {
	constexpr int HANDSHAKE_FANOUT = 32; // eight times the return ring

	spsc_queue<int, 8> commands;
	spsc_queue<int, 4> events;
	stop_handshake handshake;

	std::thread consumer([&] {
		handshake.run([&]() noexcept {
			std::size_t n = 0;
			while (std::optional<int> cmd = commands.try_dequeue()) {
				for (int e = 0; e < HANDSHAKE_FANOUT; ++e)
					while (!events.try_emplace(*cmd)) std::this_thread::yield();
				++n;
			}
			return n;
		});
	});

	ASSERT_TRUE(commands.try_emplace(7));
	int received = 0;
	handshake.stop([&]() noexcept {
		std::size_t n = 0;
		while (events.try_dequeue()) ++n;
		received += static_cast<int>(n);
		return n;
	});
	EXPECT_TRUE(handshake.has_finished());
	consumer.join();
	while (events.try_dequeue()) ++received;

	EXPECT_EQ(received, HANDSHAKE_FANOUT);
}

TEST(StopHandshake, HasNotFinishedBeforeRunReturns) {
	const stop_handshake handshake;
	EXPECT_FALSE(handshake.has_finished());
}

} // namespace
