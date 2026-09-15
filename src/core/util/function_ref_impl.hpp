// function_ref_impl.hpp
// Intentionally unguarded – included from inside namespace exchange::core::util

#if !defined(FRF_CV) || !defined(FRF_NOEXCEPT) || !defined(FRF_INVOKE_QUAL) || \
	!defined(FRF_CONSTRAINT)
#error                                                                         \
	"FRF_CV, FRF_NOEXCEPT, FRF_INVOKE_QUAL and FRF_CONSTRAINT must be defined"
#endif

template <class R, class... Args>
class EXCHANGE_POINTER function_ref<R(Args...) FRF_CV FRF_NOEXCEPT>
	: private detail::function_ref_base<R, Args...> {
	using base   = detail::function_ref_base<R, Args...>;
	using entity = typename base::object_ptr;

public:
	constexpr function_ref() noexcept EXCHANGE_DELETE(
		"a function_ref always refers to something; there is no empty state to "
		"check for. Construct it from the callable, or hold a "
		"std::optional<function_ref> if absence has to be representable.");

	constexpr function_ref(const function_ref &) noexcept            = default;
	constexpr function_ref &operator=(const function_ref &) noexcept = default;

	// A function pointer *is* the target, so it is stored by value. Routing it
	// through the F&& overload below would store the address of that
	// overload's parameter, which dies with the full-expression - `ref r =
	// &f;` would dangle immediately. Partial ordering picks this one over
	// F&& for any function pointer, which is how C++26 spells it too.
	// Not usable in a constant expression: reinterpret_cast never is.
	template <class F>
		requires std::is_function_v<F> && FRF_CONSTRAINT
	(R, F, Args)
		// NOLINTNEXTLINE(google-explicit-constructor)
		constexpr function_ref(F *f) noexcept
		: base(
			  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
			  entity(reinterpret_cast<void (*)()>(f)),
			  +[](entity obj, Args &&...args) FRF_NOEXCEPT -> R {
				  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
				  return std::invoke_r<R>(reinterpret_cast<F *>(obj.function),
										  std::forward<Args>(args)...);
			  }) {}

	// A stateless callable needs no address at all: an empty type has no
	// non-static members, so a fresh instance is indistinguishable from the
	// caller's and the thunk just default-constructs one. That makes the
	// commonest trap - `ref r = [] { ... };` - genuinely safe rather than
	// merely diagnosed, because nothing is borrowed and there is no temporary
	// left to outlive.
	//
	// Stronger than converting the closure to a function pointer, which is the
	// other way to exploit statelessness: that stores a pointer to the
	// compiler's static invoker and pays an indirection through it, where this
	// is a direct call the optimiser inlines. It also keeps working in a
	// constant expression, and covers stateless functors (std::less<>) rather
	// than only lambdas. C++20 made captureless lambdas default-constructible,
	// which is what this leans on.
	template <class F>
		requires (!std::is_same_v<std::remove_cvref_t<F>, function_ref>) &&
				 std::is_empty_v<std::remove_cvref_t<F>> &&
				 std::is_default_constructible_v<std::remove_cvref_t<F>> &&
				 FRF_CONSTRAINT
	(R, F, Args)
		// NOLINTNEXTLINE(google-explicit-constructor)
		constexpr function_ref(F &&) noexcept
		: base(
			  entity(static_cast<void *>(nullptr)),
			  +[](entity, Args &&...args) FRF_NOEXCEPT -> R {
				  using target = std::remove_cvref_t<F>;
				  target instance{};
				  return std::invoke_r<R>(
					  static_cast<FRF_INVOKE_QUAL target &>(instance),
					  std::forward<Args>(args)...);
			  }) {}

	// A pointer-to-member is a value, like a function pointer, so storing its
	// address is never what was meant - and unlike a closure there is no safe
	// case to preserve, which is why this one can be a constraint where the
	// temporary-lambda trap can only be a diagnostic. C++26 refuses it here
	// too; reach for a lambda that spells out the call.
	template <class F>
		requires (!std::is_same_v<std::remove_cvref_t<F>, function_ref>) &&
				 (!std::is_member_pointer_v<std::remove_reference_t<F>>) &&
				 (!(std::is_empty_v<std::remove_cvref_t<F>> &&
					std::is_default_constructible_v<std::remove_cvref_t<F>>)) &&
				 FRF_CONSTRAINT
	(R, F, Args)
		// NOLINTNEXTLINE(google-explicit-constructor)
		constexpr function_ref(F &&f EXCHANGE_LIFETIMEBOUND) noexcept
		: base(
			  entity(detail::erase_const(std::addressof(f))),
			  +[](entity obj, Args &&...args) FRF_NOEXCEPT -> R {
				  using target_ptr = std::add_pointer_t<
					  FRF_INVOKE_QUAL std::remove_reference_t<F>>;
				  return std::invoke_r<R>(*static_cast<target_ptr>(obj.object),
										  std::forward<Args>(args)...);
			  }) {}

	constexpr R operator()(Args... args) const FRF_NOEXCEPT {
		return this->invoke_(this->object_, std::forward<Args>(args)...);
	}

	// Deleted for exactly the arguments a constructor above would *borrow*:
	// the converting constructor is implicit, so `ref = some_lambda` would
	// bind to a temporary function_ref holding that closure's address and
	// dangle the moment the statement ended. The two exempt shapes are the two
	// that are stored by value instead - a pointer, and a stateless callable -
	// which is why this constraint mirrors those constructors rather than
	// naming them again. C++26 [func.wrap.ref.class] deletes on the same
	// principle, minus the stateless case, which it has no constructor for.
	//
	// The by-value parameter is load-bearing: with T&& an lvalue deduces a
	// reference type and is_pointer_v is false on it.
	template <class T>
		requires (!std::is_same_v<T, function_ref>) &&
				 (!std::is_pointer_v<T>) &&
				 (!(std::is_empty_v<T> && std::is_default_constructible_v<T>))
	function_ref &operator=(T) EXCHANGE_DELETE(
		"a function_ref borrows, and this callable would have to be stored by "
		"address, so the assignment would leave it dangling at the semicolon. "
		"Bind the callable to a named object that outlives the function_ref "
		"and assign that, or assign another function_ref. Function pointers "
		"and stateless (captureless) callables are stored by value and may be "
		"assigned directly.");
};

#undef FRF_CV
#undef FRF_NOEXCEPT
#undef FRF_INVOKE_QUAL
#undef FRF_CONSTRAINT
