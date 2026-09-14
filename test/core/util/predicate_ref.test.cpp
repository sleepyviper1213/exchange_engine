
#include "core/util/predicate_ref.hpp"

#include <gtest/gtest.h>

#include <type_traits>

using exchange::core::util::predicate_ref;

namespace {
TEST(PredicateRef, Contracts) {
	// The contract, pinned. Deliberately not `std::predicate`: that would hold
	// for the loose `function_ref<bool(T)>` too, so it cannot tell the two
	// apart and would not notice this alias being widened - and its
	// `regular_invocable` half asks for an equality-preserving call, which the
	// callables this is *for* (pop one element, push one element) are not. What
	// is worth asserting is the strictness itself, and especially that the
	// refusal still happens: the last line is what fails if the `const
	// noexcept` is ever dropped from the alias.
	static_assert(
		std::is_nothrow_invocable_r_v<bool, predicate_ref<int> &, int>);
	static_assert(
		std::is_constructible_v<predicate_ref<int>, bool (*)(int) noexcept>);
	static_assert(!std::is_constructible_v<predicate_ref<int>, bool (*)(int)>,
				  "a predicate that has not promised nothrow must not bind");
	SUCCEED();
}
} // namespace
