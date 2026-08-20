#pragma once
// A borrowed predicate: the strict corner of function_ref, named.
//
// `function_ref` has four specialisations, because `const` and `noexcept` are
// independent bits of a function type and each needs its own thunk. This alias
// picks one of them deliberately - the strictest - because a *predicate* is a
// question, and asking a question should neither change the thing being asked
// nor be able to fail:
//
//   const     the target is invoked through a const reference, so answering
//             cannot mutate it
//   noexcept  the answer cannot arrive as an exception
//
// The second is checked rather than assumed: the `noexcept` specialisations
// constrain on `is_nothrow_invocable_r_v`, so a callable that has not promised
// is refused at the call site instead of turning into a `std::terminate` later.
// @see function_ref.hpp
//
// --- what that means at a call site ---------------------------------------
//
// A lambda has to say so, whatever its body does - `noexcept` is part of the
// type, and the compiler will not infer it:
//
//   drain([&q](T &out) noexcept { return q.try_dequeue(out); });   // binds
//   drain([&q](T &out)          { return q.try_dequeue(out); });   // refused
//
// That refusal is the feature. Writing the specifier is the caller stating the
// promise, in the one place that knows whether it holds.
//
// --- when this is the wrong alias -----------------------------------------
//
// A callable that legitimately mutates or throws is not a predicate in this
// sense, and should spell `function_ref<bool(T)>` in full so that the weaker
// contract is visible in the signature rather than hidden behind a short name.
//
// @warning Do not mix the two spellings across one interface. The four
//          specialisations do not convert between each other - they *bind* to
//          each other, because function_ref's converting constructor excludes
//          only its own specialisation and treats any other as an ordinary
//          callable. So `function_ref<bool(T)> loose = some_predicate_ref;`
//          compiles, wraps rather than converts, and leaves you with two
//          indirections and a reference to whatever the source was pointing at.
//
// @warning Lifetime is function_ref's: pass one, never keep one. A member or a
//          closure capture of this type outlives the callable it points at in
//          every case worth writing down.

#include "core/util/function_ref.hpp"

#include <type_traits>

namespace exchange::core::util {

/// @brief A borrowed `bool(T)` that neither mutates its target nor throws.
template <typename T>
using predicate_ref = function_ref<bool(T) const noexcept>;

// The contract, pinned. Deliberately not `std::predicate`: that would hold for
// the loose `function_ref<bool(T)>` too, so it cannot tell the two apart and
// would not notice this alias being widened - and its `regular_invocable` half
// asks for an equality-preserving call, which the callables this is *for* (pop
// one element, push one element) are not. What is worth asserting is the
// strictness itself, and especially that the refusal still happens: the last
// line is what fails if the `const noexcept` is ever dropped from the alias.
static_assert(std::is_nothrow_invocable_r_v<bool, predicate_ref<int> &, int>);
static_assert(
	std::is_constructible_v<predicate_ref<int>, bool (*)(int) noexcept>);
static_assert(!std::is_constructible_v<predicate_ref<int>, bool (*)(int)>,
			  "a predicate that has not promised nothrow must not bind");

} // namespace exchange::core::util
