// move_only_function_impl.hpp
// Intentionally unguarded.
// Don't include this header directly.

#if !defined(MOF_CV) || !defined(MOF_REF) || !defined(MOF_NOEXCEPT) ||         \
	!defined(MOF_INVOKE_QUAL) || !defined(MOF_CONSTRAINT)
#error                                                                         \
	"MOF_CV, MOF_REF, MOF_NOEXCEPT, MOF_INVOKE_QUAL and MOF_CONSTRAINT must be defined"
#endif

template <class R, class... Args>
class move_only_function<R(Args...) MOF_CV MOF_REF MOF_NOEXCEPT>
	: private detail::move_only_function_base<R, Args...> {
	using base = detail::move_only_function_base<R, Args...>;

	using invoker_t = R (*)(MOF_CV detail::mof_storage &MOF_REF,
							Args...) MOF_NOEXCEPT;

public:
	using result_type = R;

	//---------------------------------------------------------------------
	// Constructors
	//---------------------------------------------------------------------
	move_only_function() noexcept = default;

	move_only_function(std::nullptr_t) noexcept {}

	move_only_function(move_only_function &&) noexcept = default;

	move_only_function(const move_only_function &) = delete;

	template <class F>
		requires (!std::is_same_v<std::remove_cvref_t<F>,
								  move_only_function>) &&
				 MOF_CONSTRAINT
	(R, std::decay_t<F>, Args) move_only_function(F &&f) {
		using VT = std::decay_t<F>;

		if constexpr (std::is_pointer_v<VT> || std::is_member_pointer_v<VT>) {
			if (f == nullptr) return;
		}

		this->template construct<VT>(std::forward<F>(f));

		this->set_invoker(+[](MOF_CV detail::mof_storage &s MOF_REF, Args... a)
							   MOF_NOEXCEPT -> R {
			if constexpr (detail::mof_use_local<VT>) {
				return std::invoke_r<R>(static_cast<MOF_INVOKE_QUAL VT &>(
											*reinterpret_cast<VT *>(s.local)),
										std::forward<Args>(a)...);
			} else {
				return std::invoke_r<R>(static_cast<MOF_INVOKE_QUAL VT &>(
											*static_cast<VT *>(s.remote)),
										std::forward<Args>(a)...);
			}
		});
	}

	template <class T, class... CArgs>
		requires std::is_constructible_v<T, CArgs...> && MOF_CONSTRAINT
	(R, T, Args) explicit move_only_function(std::in_place_type_t<T>,
											 CArgs &&...args) {
		this->template construct<T>(std::forward<CArgs>(args)...);

		this->set_invoker(+[](MOF_CV detail::mof_storage &s MOF_REF, Args... a)
							   MOF_NOEXCEPT -> R {
			if constexpr (detail::mof_use_local<T>) {
				return std::invoke_r<R>(static_cast<MOF_INVOKE_QUAL T &>(
											*reinterpret_cast<T *>(s.local)),
										std::forward<Args>(a)...);
			} else {
				return std::invoke_r<R>(static_cast<MOF_INVOKE_QUAL T &>(
											*static_cast<T *>(s.remote)),
										std::forward<Args>(a)...);
			}
		});
	}

	//---------------------------------------------------------------------
	// Assignment
	//---------------------------------------------------------------------
	move_only_function &operator=(move_only_function &&) noexcept = default;

	move_only_function &operator=(const move_only_function &) = delete;

	move_only_function &operator=(std::nullptr_t) noexcept {
		this->destroy();
		return *this;
	}

	template <class F>
		requires (!std::is_same_v<std::remove_cvref_t<F>,
								  move_only_function>) &&
				 MOF_CONSTRAINT
	(R, std::decay_t<F>, Args) move_only_function &operator=(F &&f) {
		move_only_function(std::forward<F>(f)).swap(*this);
		return *this;
	}

	//---------------------------------------------------------------------
	// Modifiers / observers
	//---------------------------------------------------------------------
	void swap(move_only_function &other) noexcept { base::swap(other); }

	using base::operator bool;

	R operator()(Args... args) MOF_CV MOF_REF MOF_NOEXCEPT {
		return this->template get_invoker<invoker_t>()(
			this->storage_,
			std::forward<Args>(args)...);
	}
};
