#include "trading-engine/execution/order_manager.hpp"

#include "trading-engine/orders/order.hpp"
#include "trading-engine/orders/types.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <unordered_map>

// Micro-benchmarks for execution::order_manager - the pre-allocated record
// store that sits above the book and remembers orders after the book has
// forgotten them. Two questions decide whether the pre-allocation was worth
// writing:
//
//  1. What does one order's whole managed lifetime cost (admit, fill, retire),
//     in steady state, once every slot is being recycled? That is the per-order
//     tax the matching path pays for having a venue-level record at all.
//  2. What does resolving one cost - by handle (a bounds check, a generation
//     compare, one line) versus by id (a hash and a probe, then that line)?
//
// The reference throughout is the same store built the obvious way: an
// std::unordered_map keyed by order id, which allocates a node per order and
// frees it per order. That is what the pool has to beat to justify existing,
// and it is also what an OMS looks like before anyone thinks about allocation.
namespace {

using exchange::order_id_t;
using exchange::quantity_t;
using exchange::side_t;
using exchange::engine::execution::order_handle;
using exchange::engine::execution::order_manager;
using exchange::engine::execution::order_record;
using exchange::engine::orders::order;

// Comfortably beyond L2 at 64 bytes a slot (2 MB), so a lookup is a real miss
// rather than a hit on a table that happens to fit in cache. A venue sizes this
// to worst-case live orders; 32k is the book's own default hint.
inline constexpr std::uint32_t CAPACITY = 1U << 15U;

[[nodiscard]] order limit(order_id_t id, quantity_t qty = 10) {
	return order{.id = id, .side = side_t::bid, .price = 100, .qty = qty};
}

// --- one order's whole managed lifetime -------------------------------------

// Steady state past capacity: every admit after the first CAPACITY orders takes
// its slot back from the retired FIFO, so this measures the recycle path (index
// erase, generation bump, index insert) and not the easy bump-pointer path.
// This is the number that matters - a venue runs here, not in its first 32k
// orders.
void BM_OrderManager_AdmitFillRetire(benchmark::State &state) {
	order_manager manager{CAPACITY};
	order_id_t next = 1;

	for (auto _ : state) {
		auto handle = manager.admit(limit(next++)); // non-const: the
		benchmark::DoNotOptimize(handle);           // const-ref DoNotOptimize
													// is deprecated
		manager.apply_fill(*handle, 10); // fills, and retires with it
	}
	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_OrderManager_AdmitFillRetire);

// The reference: the same lifetime against a node-allocating hash map. Erasing
// on termination is what a map-based store must do to stay bounded - and it is
// also what throws the history away, so this is strictly less functionality for
// strictly more work.
void BM_UnorderedMap_InsertFillErase(benchmark::State &state) {
	std::unordered_map<order_id_t, order_record> records;
	records.reserve(CAPACITY);
	order_id_t next = 1;

	for (auto _ : state) {
		const order_id_t id = next++;
		auto [entry, added] = records.emplace(
			id,
			order_record{.id        = id,
						 .timestamp = 0,
						 .state     = exchange::engine::order_state{10},
						 .symbol    = 0,
						 .account   = 0,
						 .price     = 100,
						 .side      = side_t::bid,
						 .type = exchange::engine::orders::order_type::LIMIT,
						 .tif  = exchange::engine::orders::
							 time_in_force_instruction::GOOD_TILL_CANCELLED,
						 .reason = exchange::engine::reject_reason::NONE,
						 .flags  = {}});
		benchmark::DoNotOptimize(entry->second);
		entry->second.state.apply_fill(10);
		records.erase(entry);
	}
	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_UnorderedMap_InsertFillErase);

// Admit and cancel rather than admit and fill: the same slot churn without the
// fill, so the difference between the two is what a fill costs and the shared
// part is what the record-keeping costs.
void BM_OrderManager_AdmitCancel(benchmark::State &state) {
	order_manager manager{CAPACITY};
	order_id_t next = 1;

	for (auto _ : state) {
		auto handle = manager.admit(limit(next++)); // non-const: the
		benchmark::DoNotOptimize(handle);           // const-ref DoNotOptimize
													// is deprecated
		manager.cancel(*handle);
	}
	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_OrderManager_AdmitCancel);

// --- resolution --------------------------------------------------------------

/// @brief A manager holding @p count live orders under ids 1..count.
///
/// Admitted in id order into an empty manager, so order @c n lives in slot
/// @c n-1 at generation 0 - which is what lets the lookup cases rebuild a
/// handle arithmetically instead of reading one out of a side array. That side
/// array was the first version of this benchmark, and it made the handle case
/// *slower* than the id case: a random stride through 256 kB of handles is its
/// own cache miss, and it was being charged to the table the case exists to
/// measure.
struct populated {
	order_manager manager{CAPACITY};

	explicit populated(std::uint32_t count) {
		for (order_id_t id = 1; id <= count; ++id)
			benchmark::DoNotOptimize(*manager.admit(limit(id)));
	}
};

/// @brief Step to an unrelated slot each iteration.
///
/// A large odd stride, so consecutive lookups never share a line and the
/// prefetcher has nothing to work with - and a mask rather than @c %, because
/// the divisor is a runtime value and a 64-bit division is ~20 cycles, which at
/// these sizes is most of the measurement. Every @c Range value below is a
/// power of two so the mask is exact.
[[nodiscard]] constexpr std::uint32_t step(std::uint32_t cursor,
										   std::uint32_t count) noexcept {
	return (cursor + 9973U) & (count - 1U);
}

// By handle: a bounds check, a generation compare, and the one cache line the
// slot was padded to occupy. No hashing, because the caller already knows where
// the record is - which is the whole reason the admission hands one back.
void BM_OrderManager_LookupByHandle(benchmark::State &state) {
	const auto count = static_cast<std::uint32_t>(state.range(0));
	populated fixture{count};
	std::uint32_t cursor = 0;

	for (auto _ : state) {
		cursor = step(cursor, count);
		const order_record *record =
			fixture.manager.get(order_handle{.slot = cursor, .generation = 0});
		// Read through the pointer, in both cases. A lookup that only returns
		// an address is not one anybody performs, and leaving the read out
		// charges the record's cache miss to whichever case happens to touch
		// the slot while resolving - which flattered find_record by ~10 ns.
		benchmark::DoNotOptimize(record->state.remaining());
	}
	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_OrderManager_LookupByHandle)
->RangeMultiplier(8)->Range(64, CAPACITY);

// By id: everything the handle lookup does, plus the hash and the probe that
// find the slot index first. The gap between the two is what a caller buys by
// keeping the handle it was given rather than looking the order up again.
void BM_OrderManager_LookupById(benchmark::State &state) {
	const auto count = static_cast<std::uint32_t>(state.range(0));
	populated fixture{count};
	std::uint32_t cursor = 0;

	for (auto _ : state) {
		cursor                     = step(cursor, count);
		const order_record *record = fixture.manager.find_record(cursor + 1);
		benchmark::DoNotOptimize(record->state.remaining());
	}
	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_OrderManager_LookupById)
->RangeMultiplier(8)->Range(64, CAPACITY);

// The question a client asks about an order the book has already forgotten, and
// the reason the component exists. It is a lookup plus a status switch, so it
// should cost what find_record costs - if it does not, the answer is being
// computed rather than remembered.
void BM_OrderManager_CancellableOnTerminalOrders(benchmark::State &state) {
	order_manager manager{CAPACITY};
	for (order_id_t id = 1; id <= CAPACITY; ++id) {
		const auto handle = manager.admit(limit(id));
		manager.apply_fill(*handle, 10); // every record terminal and retained
	}
	std::uint32_t cursor = 0;

	for (auto _ : state) {
		cursor = step(cursor, CAPACITY);
		benchmark::DoNotOptimize(manager.cancellable(cursor + 1));
	}
	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_OrderManager_CancellableOnTerminalOrders);

} // namespace
