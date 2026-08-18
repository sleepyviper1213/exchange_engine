#include "core/util/function_ref.hpp"

#include <gtest/gtest.h>

#include <string>
#include <type_traits>

// Four specialisations for two independent bits, and the whole point of having
// them is that each *rejects* something the others accept. So most of what is
// worth checking here is negative and belongs at compile time: which callables
// a given signature refuses, and what a qualifier actually constrains.
//
// The one thing genuinely worth running is the const/non-const distinction,
// since "invoked through a const reference" is a claim about which overload is
// selected and a static_assert cannot see that.

using exchange::core::util::function_ref;

namespace {

/// @brief Records which overload was called, so the const-ness of the
/// *invocation*
///        is observable rather than merely asserted.
struct dual {
	mutable bool called_const = false;
	bool called_mutable       = false;

	void operator()() const { called_const = true; }

	void operator()() { called_mutable = true; }
};

/// @brief A callable whose @c operator() is non-const - the shape the @c const
///        specialisations exist to exclude.
struct mutating {
	int calls = 0;

	void operator()() { ++calls; }
};

struct throwing {
	void operator()() const { throw std::string("no"); }
};

TEST(FunctionRef, CallsThroughToTheReferencedCallable) {
	int seen       = 0;
	const auto add = [&seen](int value) { seen += value; };

	const function_ref<void(int)> ref = add;
	ref(2);
	ref(3);
	EXPECT_EQ(seen, 5);
}

TEST(FunctionRef, PropagatesTheReturnValue) {
	const auto doubler               = [](int value) { return value * 2; };
	const function_ref<int(int)> ref = doubler;
	EXPECT_EQ(ref(21), 42);
}

// Arguments are forwarded, not copied: a reference parameter has to reach the
// callable as a reference or a consuming callback could not consume anything.
TEST(FunctionRef, ForwardsReferenceArgumentsSoMutationIsVisible) {
	const auto bump                     = [](int &value) { value += 1; };
	const function_ref<void(int &)> ref = bump;
	int subject                         = 41;
	ref(subject);
	EXPECT_EQ(subject, 42);
}

// The distinction the signature's `const` actually draws. Not "operator() is
// const" - that is true of every specialisation - but which overload of the
// *target* gets selected.
TEST(FunctionRef, TheSignaturesConstDecidesHowTheTargetIsInvoked) {
	dual unqualified;
	const function_ref<void()> plain = unqualified;
	plain();
	EXPECT_TRUE(unqualified.called_mutable);
	EXPECT_FALSE(unqualified.called_const);

	dual qualified;
	const function_ref<void() const> constant = qualified;
	constant();
	EXPECT_TRUE(qualified.called_const);
	EXPECT_FALSE(qualified.called_mutable);
}

// A function_ref is two pointers and meant to be passed by value; copying one
// must keep referring to the same callable.
TEST(FunctionRef, CopiesReferToTheSameCallable) {
	int seen                          = 0;
	const auto add                    = [&seen](int value) { seen += value; };
	const function_ref<void(int)> one = add;
	const function_ref<void(int)> two = one; // copy, not a rebind
	one(1);
	two(2);
	EXPECT_EQ(seen, 3);
}

TEST(FunctionRef, IsTwoPointersAndTriviallyCopyable) {
	static_assert(std::is_trivially_copyable_v<function_ref<void()>>);
	static_assert(std::is_trivially_copyable_v<function_ref<void() noexcept>>);
	static_assert(std::is_trivially_copyable_v<function_ref<void() const>>);
	static_assert(
		std::is_trivially_copyable_v<function_ref<void() const noexcept>>);
	static_assert(sizeof(function_ref<void()>) == 2 * sizeof(void *));
	static_assert(sizeof(function_ref<void() const noexcept>) ==
				  sizeof(function_ref<void()>));
	SUCCEED();
}

// The `noexcept` in the signature has to be a guarantee rather than a hope: a
// throwing callable is refused at the call site instead of terminating the
// process at the throw. This is what makes it safe for spsc_queue::consume_all,
// whose callback runs before the read cursor is published.
TEST(FunctionRef, NoexceptSpecialisationsRefuseAThrowingCallable) {
	static_assert(
		std::is_constructible_v<function_ref<void() const>, throwing>);
	static_assert(!std::is_constructible_v<function_ref<void() const noexcept>,
										   throwing>);

	const auto quiet = []() noexcept {};
	static_assert(std::is_constructible_v<function_ref<void() const noexcept>,
										  decltype(quiet) &>);
	SUCCEED();
}

TEST(FunctionRef, ANoexceptRefIsItselfNoexceptToCall) {
	const auto quiet                              = []() noexcept {};
	const function_ref<void() const noexcept> ref = quiet;
	static_assert(noexcept(ref()));
	ref();
	SUCCEED();
}

// The exclusion the const specialisations buy, stated as a test so it is a
// decision rather than an accident: a callable that mutates itself binds to the
// unqualified form and is refused by the const one.
TEST(FunctionRef, ConstSpecialisationsRefuseACallableThatMutatesItself) {
	static_assert(std::is_constructible_v<function_ref<void()>, mutating &>);
	static_assert(
		!std::is_constructible_v<function_ref<void() const>, mutating &>);

	mutating counter;
	const function_ref<void()> ref = counter;
	ref();
	ref();
	EXPECT_EQ(counter.calls, 2);
}

// Taken from C++26: rebinding to a callable is deleted, because the converting
// constructor is implicit and `ref = [] {...}` would otherwise bind to a
// temporary and dangle the moment the statement ended.
TEST(FunctionRef, RebindingToACallableIsDeleted) {
	using ref = function_ref<void()>;
	static_assert(!std::is_assignable_v<ref &, void (*)()>);
	static_assert(!std::is_assignable_v<ref &, decltype([] {})>);
	// Copying one onto another is still fine: both refer to a callable somebody
	// else is keeping alive.
	static_assert(std::is_assignable_v<ref &, const ref &>);
	SUCCEED();
}

// A callable's own const-ness is respected: passing a const object invokes the
// const overload even through the unqualified specialisation, because the
// target type is deduced from the argument.
TEST(FunctionRef, AConstCallableIsInvokedAsConst) {
	const dual subject;
	const function_ref<void()> ref = subject;
	ref();
	EXPECT_TRUE(subject.called_const);
}

TEST(FunctionRef, WorksWithAPlainFunctionPointer) {
	static int total = 0;
	total            = 0;

	struct helper {
		static void add(int value) { total += value; }
	};

	void (*pointer)(int)              = &helper::add;
	const function_ref<void(int)> ref = pointer;
	ref(7);
	EXPECT_EQ(total, 7);
}

} // namespace
