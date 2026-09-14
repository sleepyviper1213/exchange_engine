// function_ref_impl.hpp
// Intentionally unguarded – included from inside namespace exchange::core::util

#if !defined(FRF_CV) || !defined(FRF_NOEXCEPT) || !defined(FRF_INVOKE_QUAL) || \
	!defined(FRF_CONSTRAINT)
#error                                                                         \
	"FRF_CV, FRF_NOEXCEPT, FRF_INVOKE_QUAL and FRF_CONSTRAINT must be defined"
#endif

template <class R, class... Args>
class function_ref<R(Args...) FRF_CV FRF_NOEXCEPT>
	: private detail::function_ref_base<R, Args...> {
	using base = detail::function_ref_base<R, Args...>;

public:
	constexpr function_ref() noexcept = delete;

	constexpr function_ref(const function_ref &) noexcept            = default;
	constexpr function_ref &operator=(const function_ref &) noexcept = default;

	template <class F>
		requires (!std::is_same_v<std::remove_cvref_t<F>, function_ref>) &&
				 FRF_CONSTRAINT
	(R, F, Args)
		// NOLINTNEXTLINE(google-explicit-constructor)
		constexpr function_ref(F &&f) noexcept
		: base(
			  std::forward<F>(f),
			  +[](typename base::object_ptr obj, Args... args)
				   FRF_NOEXCEPT -> R {
				  using target_ptr = std::add_pointer_t<
					  FRF_INVOKE_QUAL std::remove_reference_t<F>>;
				  return std::invoke_r<R>(*static_cast<target_ptr>(obj),
										  std::forward<Args>(args)...);
			  }) {}

	constexpr R operator()(Args... args) const FRF_NOEXCEPT {
		return this->invoke_(this->object_, std::forward<Args>(args)...);
	}

	template <class Other>
		requires (!std::is_same_v<std::remove_cvref_t<Other>, function_ref>)
	function_ref &operator=(Other &&) = delete;
};
