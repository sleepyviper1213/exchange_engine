#include "core/util/function_ref.hpp"

#include <gtest/gtest.h>

#include <concepts>
#include <functional>
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
	void operator()() const { throw; }
};

TEST(FunctionRef, CallsThroughToTheReferencedCallable) {
	int seen         = 0;
	auto add_to_seen = [&seen](int value) { seen += value; };

	// Named: this one captures, so the function_ref borrows its address.
	const function_ref<void(int)> add = add_to_seen;
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
	int seen        = 0;
	auto accumulate = [&seen](int value) { seen += value; };

	// The lambda has to be a named object: a function_ref borrows, so binding
	// one to a temporary closure leaves it dangling at the semicolon.
	const function_ref<void(int)> one = accumulate;
	const function_ref<void(int)> two = one; // copy, not a rebind
	one(1);
	two(2);
	EXPECT_EQ(seen, 3);
}

TEST(FunctionRef, IsTwoPointersAndTriviallyCopyable) {
	using ref = function_ref<void()>;
	static_assert(std::copyable<ref>);
	static_assert(std::is_trivially_copyable_v<ref>);
	static_assert(std::is_trivially_copyable_v<function_ref<void() noexcept>>);
	static_assert(std::is_trivially_copyable_v<function_ref<void() const>>);
	static_assert(
		std::is_trivially_copyable_v<function_ref<void() const noexcept>>);
	static_assert(std::is_nothrow_copy_assignable_v<ref>);
	static_assert(
		sizeof(ref) == 2 * sizeof(void *),
		"a function_ref is a pointer to the callable and a pointer to the "
		"thunk, and nothing else");
	static_assert(
		sizeof(function_ref<void() noexcept>) == sizeof(ref),
		"the noexcept variant differs in the thunk's type, not its size");
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

// Taken from C++26: rebinding is deleted for a callable that would have to be
// *borrowed*, because the converting constructor is implicit and
// `ref = some_capturing_lambda` would otherwise bind to a temporary and dangle
// the moment the statement ended. What stays assignable is what gets stored by
// value instead.
TEST(FunctionRef, RebindingToABorrowedCallableIsDeleted) {
	using ref    = function_ref<void()>;
	int captured = 0;

	// Borrowed - refused.
	static_assert(
		!std::is_assignable_v<ref &, decltype([captured] { (void)captured; })>);
	// Stored by value - permitted.
	static_assert(std::is_assignable_v<ref &, void (*)()>);
	static_assert(std::is_assignable_v<ref &, decltype([] {})>);
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

// The pointer above was a named lvalue, which the `F&&` constructor can store
// the address of safely. A prvalue cannot be: it dies with the full-expression,
// so `function_ref r = &f;` kept the address of a dead parameter and every
// later call read a reused stack slot - a segfault once something else claims
// it. The C++26 `function_ref(F*)` overload exists for exactly this and copies
// the function pointer itself.
//
// Without the fix this file does not compile at all: the rebind below is the
// assignment C++26 leaves legal precisely because a pointer has no lifetime to
// outlive, and the old blanket-deleted `operator=` rejected it. The
// construction is the undefined half, so run this one under ASan to see it.
// A stateless callable is not borrowed at all. An empty type has no
// non-static members, so the thunk default-constructs a fresh one and there is
// no address to outlive - which makes the commonest spelling of all, binding
// to a temporary lambda, genuinely safe rather than merely diagnosed.
TEST(FunctionRef, AStatelessCallableIsNotBorrowed) {
	const function_ref<int()> from_temporary = [] { return 7; };
	EXPECT_EQ(from_temporary(), 7);

	// Not a lambda-only path: any empty, default-constructible callable.
	const function_ref<bool(int, int) const> less = std::less<int>{};
	EXPECT_TRUE(less(1, 2));

	// And it reaches where the borrowing path cannot - a constant expression,
	// which the address path rules out.
	constexpr function_ref<int(int) const> add_one = [](int x) {
		return x + 1;
	};
	static_assert(add_one(41) == 42);

	// A capturing lambda is not empty, so it still takes the borrowed path.
	int seen        = 0;
	auto accumulate = [&seen](int value) { seen += value; };
	static_assert(!std::is_empty_v<decltype(accumulate)>);
	const function_ref<void(int)> borrowed = accumulate;
	borrowed(3);
	EXPECT_EQ(seen, 3);
}

TEST(FunctionRef, StoresAFunctionPassedAsATemporaryPointerByValue) {
	static int fnptr_total = 0;
	fnptr_total            = 0;

	struct helper {
		static void add_two(int value) { fnptr_total += 2 * value; }

		static void add_ten(int value) { fnptr_total += 10 * value; }
	};

	function_ref<void(int)> ref = &helper::add_two;
	ref(1);
	EXPECT_EQ(fnptr_total, 2);

	// Rebinding through a prvalue too - this is the assignment C++26 leaves
	// legal precisely because a pointer has no lifetime to outlive.
	ref = &helper::add_ten;
	ref(1);
	EXPECT_EQ(fnptr_total, 12);
}

TEST(FunctionRef, CopyAssignmentRebindsTargetWithoutModifyingOriginalCallable) {
	// 1. Setup two distinct callables with trackable state
	int callable_a_invocations = 0;
	int callable_b_invocations = 0;

	auto lambda_a = [&callable_a_invocations]() {
		++callable_a_invocations;
		return 100;
	};

	auto lambda_b = [&callable_b_invocations]() {
		++callable_b_invocations;
		return 200;
	};

	// 2. Bind two function_refs to different targets
	function_ref<int()> ref_a = lambda_a;
	function_ref<int()> ref_b = lambda_b;

	// Verify initial binding state
	EXPECT_EQ(ref_a(), 100);
	EXPECT_EQ(ref_b(), 200);
	EXPECT_EQ(callable_a_invocations, 1);
	EXPECT_EQ(callable_b_invocations, 1);

	// 3. Perform copy assignment (ref_a = ref_b)
	// This executes the defaulted copy-assignment operator, shallowly copying
	// the internal thunk-ptr and bound-entity pointer.
	ref_a = ref_b;

	// 4. Assert rebind behavior:
	// Invoking `ref_a` must now execute `lambda_b`, proving it was rebound.
	EXPECT_EQ(ref_a(), 200);

	// Assert that lambda_b was invoked, while lambda_a was untouched.
	EXPECT_EQ(callable_a_invocations, 1);
	EXPECT_EQ(callable_b_invocations, 2);

	// 5. Verify that changing ref_a does not alter ref_b's behavior
	EXPECT_EQ(ref_b(), 200);
	EXPECT_EQ(callable_b_invocations, 3);
}

class Calculator {
public:
	int add(int x, int y) const { return x + y; }

	int multiply(int x, int y) const { return x * y; }
};

TEST(FunctionRef, CopyAssignmentRebindsMemberFunction) {
	Calculator calc;

	// A pointer-to-member is refused outright - it is a value, so binding one
	// through `F&&` could only ever store the address of a temporary. A
	// lambda spelling out the call is the replacement, and it is named so it
	// outlives the refs that borrow it.
	static_assert(!std::is_constructible_v<
				  function_ref<int(const Calculator &, int, int)>,
				  decltype(&Calculator::add)>);

	auto add = [](const Calculator &c, int x, int y) { return c.add(x, y); };
	auto multiply = [](const Calculator &c, int x, int y) {
		return c.multiply(x, y);
	};

	function_ref<int(const Calculator &, int, int)> ref_op1 = add;
	function_ref<int(const Calculator &, int, int)> ref_op2 = multiply;

	// Verify initial target invocations
	EXPECT_EQ(ref_op1(calc, 3, 4), 7);  // 3 + 4
	EXPECT_EQ(ref_op2(calc, 3, 4), 12); // 3 * 4

	// 2. Perform copy assignment (rebind ref_op1 to point to multiply)
	ref_op1 = ref_op2;

	// 3. Assert ref_op1 now executes Calculator::multiply
	EXPECT_EQ(ref_op1(calc, 3, 4), 12);
}

TEST(FunctionRef, BoundObjectInstanceRebinding) {
	Calculator calc_a;
	Calculator calc_b;

	// You can also bind an explicit object instance alongside a member function
	// using std::nontype or lambda wrappers.
	auto via_a = [&calc_a](int x, int y) { return calc_a.add(x, y); };
	auto via_b = [&calc_b](int x, int y) { return calc_b.multiply(x, y); };

	function_ref<int(int, int)> ref_bound_a = via_a;
	function_ref<int(int, int)> ref_bound_b = via_b;

	EXPECT_EQ(ref_bound_a(5, 2), 7);
	EXPECT_EQ(ref_bound_b(5, 2), 10);

	// Rebind ref_bound_a to ref_bound_b (copies both the thunk and the calc_b
	// reference)
	ref_bound_a = ref_bound_b;

	// ref_bound_a now calls calc_b.multiply
	EXPECT_EQ(ref_bound_a(5, 2), 10);
}

// Returning a function_ref from a function is safe when - and only when - the
// callable is stateless. This one is captureless, so nothing is borrowed and
// there is no local left to outlive; a capturing lambda here would dangle, and
// clang's -Werror=dangling rejects that spelling.
function_ref<int(int)> stateless_doubler() {
	auto local_lambda = [](int x) { return x * 2; };
	return local_lambda;
}

TEST(FunctionRef, AStatelessCallableSurvivesTheScopeItWasMadeIn) {
	const auto doubled = stateless_doubler();
	EXPECT_EQ(doubled(5), 10);
}

// Assignment is deleted for the arguments that would be *borrowed*, so the two
// shapes stored by value stay assignable. A stateless lambda is one of them:
// there is no closure whose address could go stale.
TEST(FunctionRef, AStatelessCallableIsAssignable) {
	function_ref<int(int)> ref = [](int x) { return x * 2; };
	EXPECT_EQ(ref(5), 10);

	{
		// Not a dangle: the temporary owns no state to be destroyed with.
		ref = [](int x) { return x * 3; };
	}
	EXPECT_EQ(ref(5), 15);

	static_assert(std::is_assignable_v<function_ref<int(int)> &,
									   decltype([](int x) { return x; })>);
	// A capturing one is still refused - that assignment really would dangle.
	int captured = 0;
	static_assert(!std::is_assignable_v<function_ref<int(int)> &,
										decltype([captured](int x) {
											return x + captured;
										})>);
}

} // namespace
