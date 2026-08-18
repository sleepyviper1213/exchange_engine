#include "core/concurrency/synchronisation/hazard_pointer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <thread>
#include <utility>

using namespace exchange::core::concurrency::synchronisation;

namespace {
// A retirable object that reports its own destruction, so tests can assert
// exactly when reclamation happens.
struct Tracked : hazard_pointer_obj_base<Tracked> {
	std::atomic<int> *counter;
	int id;

	Tracked(std::atomic<int> *c, int i) : counter(c), id(i) {}

	// Non-copyable, non-movable on purpose. This type is the measurement
	// instrument for every reclamation assertion in this file, so a stray copy
	// would inflate the destruction count and quietly invalidate all of them.
	Tracked(const Tracked &)            = delete;
	Tracked &operator=(const Tracked &) = delete;
	Tracked(Tracked &&)                 = delete;
	Tracked &operator=(Tracked &&)      = delete;

	~Tracked() { counter->fetch_add(1, std::memory_order_relaxed); }
};

// --------------------------------------------------------------------------
// hazard_pointer handle basics
// --------------------------------------------------------------------------

TEST(HazardPointer, DefaultConstructedIsEmpty) {
	hazard_pointer hp;
	EXPECT_TRUE(hp.empty());
}

TEST(HazardPointer, MakeYieldsNonEmpty) {
	hazard_pointer hp = make_hazard_pointer();
	EXPECT_FALSE(hp.empty());
}

TEST(HazardPointer, MoveTransfersOwnership) {
	hazard_pointer a = make_hazard_pointer();
	hazard_pointer b = std::move(a);
	// Inspecting the moved-from handle is the point: hazard_pointer's move
	// constructor documents that it leaves the source empty, and that
	// guarantee is what makes the handle safe to destroy or reassign after a
	// move.
	// NOLINTNEXTLINE(bugprone-use-after-move)
	EXPECT_TRUE(a.empty());
	EXPECT_FALSE(b.empty());
}

TEST(HazardPointer, ProtectReturnsCurrentValue) {
	int value = 42;
	std::atomic<int *> src{&value};
	hazard_pointer hp = make_hazard_pointer();
	EXPECT_EQ(hp.protect(src), &value);
}

TEST(HazardPointer, ArrayAcquiresNonEmptyHandles) {
	const hazard_pointer_array<3> arr;
	static_assert(hazard_pointer_array<3>::size() == 3U);
	for (std::size_t i = 0; i < hazard_pointer_array<3>::size(); ++i)
		EXPECT_FALSE(arr[i].empty()); // exercises the const operator[]
}

// The point of an array is that its slots are *separate* records: N pointers
// protected at once, each releasable on its own. Asserting only that the
// handles are non-empty would pass even if all N aliased one record, so this
// drives them against distinct objects and releases one at a time. It also
// covers the explicit-domain constructor, which nothing else reaches.
TEST(HazardPointer, ArraySlotsProtectAndReleaseIndependently) {
	hazard_pointer_domain domain;
	std::atomic<int> destroyed{0};

	std::array<Tracked *, 3> objects{};
	std::array<std::atomic<Tracked *>, 3> sources{};
	hazard_pointer_array<3> handles(domain);

	for (std::size_t i = 0; i < objects.size(); ++i) {
		objects[i] = new Tracked(&destroyed, static_cast<int>(i));
		sources[i].store(objects[i], std::memory_order_relaxed);
		ASSERT_EQ(handles[i].protect(sources[i]), objects[i]);
	}

	for (Tracked *obj : objects) obj->retire(domain);
	domain.cleanup();
	ASSERT_EQ(destroyed.load(), 0) << "each slot must protect its own object";

	// Releasing slot 1 must free exactly object 1 - if the slots shared a
	// record, this would free all three (or none).
	handles[1].reset_protection();
	domain.cleanup();
	EXPECT_EQ(destroyed.load(), 1) << "one release freed the wrong number";

	handles[0].reset_protection();
	handles[2].reset_protection();
	domain.cleanup();
	EXPECT_EQ(destroyed.load(), 3);
}

// --------------------------------------------------------------------------
// Reclamation semantics against an explicit domain
// --------------------------------------------------------------------------

TEST(HazardPointer, ProtectedObjectSurvivesUntilReleased) {
	hazard_pointer_domain domain;
	std::atomic<int> destroyed{0};

	auto *obj = new Tracked(&destroyed, 1);
	std::atomic<Tracked *> src{obj};

	hazard_pointer hp = make_hazard_pointer(domain);
	Tracked *p        = hp.protect(src);
	ASSERT_EQ(p, obj);

	src.store(nullptr, std::memory_order_relaxed);
	obj->retire(domain);
	domain.cleanup();
	// Still protected -> must not be reclaimed.
	EXPECT_EQ(destroyed.load(), 0);

	hp.reset_protection();
	domain.cleanup();
	EXPECT_EQ(destroyed.load(), 1);
}

TEST(HazardPointer, UnprotectedObjectIsReclaimed) {
	hazard_pointer_domain domain;
	std::atomic<int> destroyed{0};

	auto *obj = new Tracked(&destroyed, 1);
	obj->retire(domain);
	domain.cleanup();
	EXPECT_EQ(destroyed.load(), 1);
}

// retire() is supposed to reclaim on its own once the backlog crosses the
// domain's threshold (2 * live records + MIN_RECLAIM). Every other test here
// drives reclamation with an explicit cleanup(), so the automatic path - the
// only one production ever takes - had no coverage at all. With no hazard
// pointer outstanding nothing is protected, so retiring well past the
// threshold must free the backlog without anyone asking.
TEST(HazardPointer, RetirePastThresholdReclaimsWithoutCleanup) {
	hazard_pointer_domain domain;
	std::atomic<int> destroyed{0};

	constexpr int kWellPastThreshold = 64;
	for (int i = 0; i < kWellPastThreshold; ++i)
		(new Tracked(&destroyed, i))->retire(domain);

	// Deliberately not an exact count: how many survive depends on where the
	// last batch fell relative to the threshold, which is an implementation
	// detail. The contract is that retire() reclaims unaided.
	EXPECT_GT(destroyed.load(), 0)
		<< "retire() never triggered a batched scan on its own";
}

// protect() must republish and re-validate until the protection it announced
// still matches the source. A single-threaded test can never enter that loop -
// nothing changes src between the store and the reload - so the retry branch
// of try_protect() was unreachable by the suite. A writer flipping src between
// two live objects makes it reachable. What must hold on every iteration is
// that protect() returns one of the two real objects: a torn or stale third
// value would mean the handle published protection for something else.
TEST(HazardPointer, ProtectRetriesUntilPublicationMatchesSource) {
	int first  = 1;
	int second = 2;
	std::atomic<int *> src{&first};
	std::atomic<bool> stop{false};

	std::thread writer([&] {
		while (!stop.load(std::memory_order_relaxed)) {
			src.store(src.load(std::memory_order_relaxed) == &first ? &second
																	: &first,
					  std::memory_order_relaxed);
		}
	});

	hazard_pointer hp    = make_hazard_pointer();
	int unexpected       = 0;
	constexpr int kSpins = 200'000;
	for (int i = 0; i < kSpins; ++i) {
		const int *observed = hp.protect(src);
		if (observed != &first && observed != &second) ++unexpected;
	}

	stop.store(true, std::memory_order_relaxed);
	writer.join();
	EXPECT_EQ(unexpected, 0)
		<< "protect() returned a pointer that was never "
		   "in src - the retry loop published a stale value";
}

TEST(HazardPointer, DomainDestructorDrainsRetired) {
	std::atomic<int> destroyed{0};
	{
		hazard_pointer_domain domain;
		for (int i = 0; i < 5; ++i)
			(new Tracked(&destroyed, i))->retire(domain);
		// No cleanup() call: rely on the destructor to drain.
	}
	EXPECT_EQ(destroyed.load(), 5);
}
} // namespace
