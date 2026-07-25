#include "concurrency/synchronisation/hazard_pointer.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>

using concurrency::synchronisation::hazard_pointer;
using concurrency::synchronisation::hazard_pointer_array;
using concurrency::synchronisation::hazard_pointer_domain;
using concurrency::synchronisation::hazard_pointer_obj_base;
using concurrency::synchronisation::make_hazard_pointer;

namespace {
// A retirable object that reports its own destruction, so tests can assert
// exactly when reclamation happens.
struct Tracked : hazard_pointer_obj_base<Tracked> {
	std::atomic<int> *counter;
	int id;

	Tracked(std::atomic<int> *c, int i) : counter(c), id(i) {}

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
	EXPECT_TRUE(a.empty());
	EXPECT_FALSE(b.empty());
}

TEST(HazardPointer, ProtectReturnsCurrentValue) {
	int value = 42;
	std::atomic<int *> src{&value};
	hazard_pointer hp = make_hazard_pointer();
	EXPECT_EQ(hp.protect(src), &value);
}

TEST(HazardPointer, ArrayAcquiresIndependentHandles) {
	hazard_pointer_array<3> arr;
	EXPECT_EQ(arr.size(), 3u);
	for (std::size_t i = 0; i < arr.size(); ++i) EXPECT_FALSE(arr[i].empty());
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
