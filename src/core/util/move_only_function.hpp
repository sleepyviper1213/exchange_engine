#pragma once

#include <functional>

#ifdef __cpp_lib_move_only_function
namespace exchange::core::util {
using std::move_only_function;
}
#else
#include "core/util/attributes.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace exchange::core::util {

namespace detail {

// ---------------------------------------------------------------------------
// Shared storage & manager
// ---------------------------------------------------------------------------
inline constexpr std::size_t MOF_SBO_SIZE      = 3 * sizeof(void *);
inline constexpr std::size_t MOF_SBO_ALIGNMENT = alignof(void *);

union mof_storage {
	alignas(MOF_SBO_ALIGNMENT) std::byte local[MOF_SBO_SIZE];
	void *remote;
};

enum class mof_op : std::uint8_t { destroy, move };

using mof_manager = void (*)(mof_op, mof_storage *, mof_storage *) noexcept;

inline void mof_empty_manager(mof_op /*unused*/, mof_storage * /*unused*/,
							  mof_storage * /*unused*/) noexcept {}

template <class T>
inline constexpr bool mof_use_local =
	sizeof(T) <= MOF_SBO_SIZE && alignof(T) <= MOF_SBO_ALIGNMENT &&
	std::is_nothrow_move_constructible_v<T>;

// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)

template <class R, class... Args>
class move_only_function_base {
protected:
	// NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
	mof_storage storage_{};
	mof_manager manager_ = mof_empty_manager;
	void *invoker_       = nullptr;
	// NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

	move_only_function_base() noexcept = default;

	move_only_function_base(move_only_function_base &&other) noexcept
		: manager_(std::exchange(other.manager_, mof_empty_manager)),
		  invoker_(std::exchange(other.invoker_, nullptr)) {
		manager_(mof_op::move, &other.storage_, &storage_);
	}

	~move_only_function_base() { destroy(); }

	move_only_function_base &
	operator=(move_only_function_base &&other) noexcept {
		if (this != &other) {
			destroy();
			manager_ = std::exchange(other.manager_, mof_empty_manager);
			invoker_ = std::exchange(other.invoker_, nullptr);
			manager_(mof_op::move, &other.storage_, &storage_);
		}
		return *this;
	}

	void destroy() noexcept {
		manager_(mof_op::destroy, &storage_, nullptr);
		manager_ = mof_empty_manager;
		invoker_ = nullptr;
	}

	void swap(move_only_function_base &other) noexcept {
		if (this == &other) return;

		mof_storage tmp{};
		manager_(mof_op::move, &storage_, &tmp);

		auto tmp_mgr  = std::exchange(manager_, other.manager_);
		auto *tmp_inv = std::exchange(invoker_, other.invoker_);

		other.manager_(mof_op::move, &other.storage_, &storage_);
		other.manager_ = tmp_mgr;
		other.invoker_ = tmp_inv;

		tmp_mgr(mof_op::move, &tmp, &other.storage_);
	}

	template <class VT, class... CArgs>
	void construct(CArgs &&...args) {
		if constexpr (mof_use_local<VT>) {
			std::construct_at(reinterpret_cast<VT *>(storage_.local),
							  std::forward<CArgs>(args)...);

			manager_ =
				[](mof_op op, mof_storage *src, mof_storage *dst) noexcept {
					VT *s = reinterpret_cast<VT *>(src->local);
					if (op == mof_op::destroy) {
						std::destroy_at(s);
					} else {
						std::construct_at(reinterpret_cast<VT *>(dst->local),
										  std::move(*s));
						std::destroy_at(s);
					}
				};
		} else {
			storage_.remote = new VT(std::forward<CArgs>(args)...);

			manager_ =
				[](mof_op op, mof_storage *src, mof_storage *dst) noexcept {
					VT *s = static_cast<VT *>(src->remote);
					if (op == mof_op::destroy) {
						delete s;
					} else {
						dst->remote = s;
						src->remote = nullptr;
					}
				};
		}
	}

	template <class Invoker>
	void set_invoker(Invoker inv) noexcept {
		invoker_ = reinterpret_cast<void *>(inv);
	}

	template <class Invoker>
	Invoker get_invoker() const noexcept {
		return reinterpret_cast<Invoker>(invoker_);
	}

public:
	move_only_function_base(const move_only_function_base &) = delete;
	move_only_function_base &
	operator=(const move_only_function_base &) = delete;

	explicit operator bool() const noexcept { return invoker_ != nullptr; }
};

} // namespace detail

template <class...>
class move_only_function;

// NOLINTBEGIN(bugprone-macro-parentheses)
//  ===========================================================================
//  1. R(Args...)
//  ===========================================================================
#define MOF_CV
#define MOF_REF
#define MOF_NOEXCEPT
#define MOF_INVOKE_QUAL
#define MOF_CONSTRAINT(R, F, Args) std::is_invocable_r_v<R, F &, Args...>

#include "move_only_function_impl.hpp"


// ===========================================================================
// 2. R(Args...) noexcept
// ===========================================================================
#define MOF_CV
#define MOF_REF
#define MOF_NOEXCEPT noexcept
#define MOF_INVOKE_QUAL
#define MOF_CONSTRAINT(R, F, Args)                                             \
	std::is_nothrow_invocable_r_v<R, F &, Args...>

#include "move_only_function_impl.hpp"


// ===========================================================================
// 3. R(Args...) const
// ===========================================================================
#define MOF_CV const
#define MOF_REF
#define MOF_NOEXCEPT
#define MOF_INVOKE_QUAL const
#define MOF_CONSTRAINT(R, F, Args) std::is_invocable_r_v<R, const F &, Args...>

#include "move_only_function_impl.hpp"

// ===========================================================================
// 4. R(Args...) const noexcept
// ===========================================================================
#define MOF_CV const
#define MOF_REF
#define MOF_NOEXCEPT noexcept
#define MOF_INVOKE_QUAL const
#define MOF_CONSTRAINT(R, F, Args)                                             \
	std::is_nothrow_invocable_r_v<R, const F &, Args...>

#include "move_only_function_impl.hpp"

// NOLINTEND(bugprone-macro-parentheses)
// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

// ---------------------------------------------------------------------------
// Non-member helpers
// ---------------------------------------------------------------------------
template <class R, class... Args>
void swap(move_only_function<R(Args...)> &a,
		  move_only_function<R(Args...)> &b) noexcept {
	a.swap(b);
}

template <class R, class... Args>
void swap(move_only_function<R(Args...) noexcept> &a,
		  move_only_function<R(Args...) noexcept> &b) noexcept {
	a.swap(b);
}

template <class R, class... Args>
void swap(move_only_function<R(Args...) const> &a,
		  move_only_function<R(Args...) const> &b) noexcept {
	a.swap(b);
}

template <class R, class... Args>
void swap(move_only_function<R(Args...) const noexcept> &a,
		  move_only_function<R(Args...) const noexcept> &b) noexcept {
	a.swap(b);
}

template <class R, class... Args>
bool operator==(const move_only_function<R(Args...)> &f,
				std::nullptr_t) noexcept {
	return !static_cast<bool>(f);
}

template <class R, class... Args>
bool operator==(std::nullptr_t,
				const move_only_function<R(Args...)> &f) noexcept {
	return !static_cast<bool>(f);
}

} // namespace exchange::core::util
#endif
