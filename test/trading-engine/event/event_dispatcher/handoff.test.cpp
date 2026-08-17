#include "event_dispatcher.fixture.hpp"
#include "trading-engine/execution/engine_partition.hpp"
#include "trading-engine/event/event_channel.hpp"
#include "trading-engine/event/event_dispatcher.hpp"
#include "trading-engine/event/command.hpp"
#include "trading-engine/orders/side.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <set>
#include <thread>
#include <vector>

// The whole loop, on two real threads: commands out through the partition's queue,
// events back through the channel, both directions lossless and both narrow enough
// to make the other side wait.
//
// The single-writer design has no synchronisation anywhere except these two rings,
// so this is the only place in the engine where a data race could exist, and the
// only test that exercises both of them at once. What it can catch: an event lost
// or duplicated when the return ring fills, and events for one listing arriving out
// of the order the engine produced them in. What it cannot: a memory-ordering bug
// that this machine's ordering happens to hide. The argument for the ordering is
// spsc_queue's — every event copied into the ring is published by the release store
// in publish_write and read through the matching acquire load, so the events a pump
// sees are exactly those the publishing thread finished writing. Windows has no
// ThreadSanitizer; the TSan run belongs to the macOS/Linux presets.
//
// If it failed, it would fail as a mismatched id set (something dropped or doubled)
// or as an out-of-order id within one listing — not as a crash, which is why both
// are asserted rather than relying on the run completing.

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::event;
using namespace exchange::engine::execution;
using namespace exchange::engine::orders;

namespace {

/// Deliberately tiny, so the engine thread has to stall and retry rather than
/// getting a free ride on a ring nobody could fill.
constexpr std::size_t RETURN_RING = 8;
constexpr std::size_t ORDERS      = 500;
constexpr symbol_id_t LEFT        = 1;
constexpr symbol_id_t RIGHT       = 2;

// Every id this listing's deliveries mentioned, in the order they arrived.
std::vector<order_id_t> ids_for(const recording_handler &handler,
								symbol_id_t symbol) {
	std::vector<order_id_t> ids;
	for (const delivery &d : handler.seen())
		if (d.symbol == symbol && (ids.empty() || ids.back() != d.id))
			ids.push_back(d.id);
	return ids;
}

TEST(EventDispatcherHandoff, EveryPublishedEventCrossesExactlyOnceAndInOrder) {
	engine_partition<256> partition(nullptr);
	partition.listing(LEFT);
	partition.listing(RIGHT);

	event_channel<RETURN_RING> channel;
	recording_handler handler;
	auto route = route_events<8>(channel, handler);

	// Set by the engine thread once it has published everything, and read by this
	// one only after seeing finished_ — so the release/acquire pair on finished_ is
	// what makes total_ safe to read without an atomic of its own.
	std::atomic<bool> stop{false};
	std::atomic<bool> finished{false};
	std::uint64_t total = 0;

	std::thread engine([&] {
		std::size_t applied = 0;
		for (;;) {
			applied += partition.drain();
			// Lossless in both directions: the batch is staged whether or not it
			// fits, and retry finishes it as the far side makes room. Yielding
			// rather than spinning, because the thread that frees the ring is the
			// one this test is sharing a core with.
			if (!channel.publish(partition.runs(),
								 partition.trades(),
								 partition.outcomes()))
				while (!channel.retry()) std::this_thread::yield();
			partition.flush();
			if (stop.load(std::memory_order_acquire) && applied == ORDERS) break;
			std::this_thread::yield();
		}
		total = channel.published();
		finished.store(true, std::memory_order_release);
	});

	// Producer: submit, and pump the return path as we go. Not optional — a
	// producer that only submitted would deadlock against a full return ring,
	// which is the honest shape of a loop that has to serve both directions.
	for (order_id_t id = 1; id <= ORDERS; ++id) {
		const auto symbol   = id % 2 == 0 ? LEFT : RIGHT;
		// Descending prices on each side, so nothing crosses and each command's
		// output is attributable to exactly one order.
		const auto price = static_cast<price_t>(1000 - id);
		const command cmd = command::place({.id        = id,
											.symbol_id = symbol,
											.side      = side_t::bid,
											.price     = price,
											.qty       = 5});
		while (!partition.submit(cmd)) {
			route.pump_all();
			std::this_thread::yield();
		}
		route.pump_all();
	}
	stop.store(true, std::memory_order_release);

	// Keep pumping until the engine says it is done *and* everything it published
	// has been delivered. Both conditions matter: the first alone would race the
	// last batch, the second alone would never terminate.
	for (;;) {
		route.pump_all();
		if (finished.load(std::memory_order_acquire) &&
			route.delivered() == total)
			break;
		std::this_thread::yield();
	}
	engine.join();

	EXPECT_GT(total, 0U);
	EXPECT_EQ(route.delivered(), total);
	EXPECT_FALSE(route.is_stalled());
	EXPECT_FALSE(channel.has_pending());
	EXPECT_EQ(channel.queued(), 0U);

	// Nothing lost and nothing doubled: every order placed produced at least one
	// event, on its own listing, and no id appeared that was never submitted.
	const std::vector<order_id_t> left  = ids_for(handler, LEFT);
	const std::vector<order_id_t> right = ids_for(handler, RIGHT);
	EXPECT_EQ(left.size() + right.size(), ORDERS);

	std::set<order_id_t> submitted_left;
	std::set<order_id_t> submitted_right;
	for (order_id_t id = 1; id <= ORDERS; ++id)
		(id % 2 == 0 ? submitted_left : submitted_right).insert(id);
	EXPECT_EQ(std::set<order_id_t>(left.begin(), left.end()), submitted_left);
	EXPECT_EQ(std::set<order_id_t>(right.begin(), right.end()), submitted_right);

	// And in order within each listing — the property a stall, a retry or a batch
	// boundary could each have broken on its own.
	EXPECT_TRUE(std::ranges::is_sorted(left));
	EXPECT_TRUE(std::ranges::is_sorted(right));

	// The narrow ring was the point: if the engine never stalled, this test proved
	// less than it claims to.
	EXPECT_GT(channel.stalls(), 0U)
		<< "a ring of " << RETURN_RING << " should have filled at least once";
}

} // namespace
