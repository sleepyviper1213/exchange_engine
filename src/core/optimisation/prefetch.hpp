#pragma once
// Prefetch hints, and the one constraint that shapes the interface.
//
// Both spellings of the intrinsic demand that the locality and read/write
// arguments be *constants*. Forwarding a function parameter straight to them
// does not compile: clang rejects every instantiation, even one whose caller
// passes nothing but the defaults ("argument to '__builtin_prefetch' must be a
// constant integer"), and GCC accepts it only until a caller passes something
// non-constant, at which point it errors inside this header and identifies the
// guilty call site through an "inlined from" note, if at all.
//
// A switch is what lets the parameters stay parameters: every arm hands the
// intrinsic a literal, so the constraint is satisfied eight times over rather
// than dodged. This is what the header originally reached for and got half
// right - it switched on the locality and passed the read/write flag through as
// a variable, so the arms it did write could not save it. The arms go through
// detail::issue so the per-compiler wall is written once instead of once each.
//
// What that costs, measured with GCC -O2 on x86-64: nothing at a call site that
// passes literals - the switch folds and a single prefetch instruction is left.
// At -O0 it is a real branch, which is the usual -O0 bargain. A caller that
// picks its hint at run time now can, and pays for the choice it asked for.

#include "core/concurrency/cache.hpp" // CACHE_LINE_SIZE

#include <cstddef>
#include <cstdint>

// clang-cl defines both _MSC_VER and __clang__ and has __builtin_prefetch, so
// the GNU test comes first everywhere it appears. MSVC targeting arm64 has no
// _mm_prefetch at all, hence the architecture half.
#if !defined(__GNUC__) && !defined(__clang__) && defined(_MSC_VER) &&          \
	(defined(_M_X64) || defined(_M_IX86))
#include <intrin.h> // _mm_prefetch, _MM_HINT_*
#endif

namespace exchange::core::optimisation {

/**
 * @brief A hint indicating how soon the prefetched line is wanted, and
 *        therefore how close in it should be pulled.
 *
 * The names say what the caller knows rather than which cache level the
 * hardware picks - a hint is not a placement instruction, and no architecture
 * here promises one.
 *
 * @note The values are @c __builtin_prefetch's own locality encoding, so the
 *       cast in @c prefetch is a reinterpretation of nothing: 0 is "no
 *       temporal locality", 3 is "high". The MSVC path maps them explicitly
 *       because @c _MM_HINT_* does not share the ordering.
 */
enum class locality_hint : std::uint8_t {
	/// @brief Streaming: touched once and not wanted again. Pulled in without
	///        evicting what the working set is using (@c _MM_HINT_NTA).
	none = 0,
	/// @brief Wanted again, but not immediately.
	low = 1,
	/// @brief Wanted again soon.
	moderate = 2,
	/// @brief Wanted immediately and repeatedly - pull it as close as the
	///        hardware allows (@c _MM_HINT_T0).
	high = 3,
};

/// @brief What the line will be touched for. Values are
/// @c __builtin_prefetch's @c rw encoding.
enum class operation : std::uint8_t { read = 0, write = 1 };

namespace detail {
#if !defined(__GNUC__) && !defined(__clang__) && defined(_MSC_VER) &&          \
	(defined(_M_X64) || defined(_M_IX86))
/// @brief A locality in @c _mm_prefetch's encoding, which orders its temporal
/// hints the other way round and spells the streaming one apart.
template <int Locality>
inline constexpr int MSVC_PREFETCH_HINT = Locality == 0   ? _MM_HINT_NTA
										  : Locality == 1 ? _MM_HINT_T2
										  : Locality == 2 ? _MM_HINT_T1
														  : _MM_HINT_T0;
#endif

/// @brief The intrinsic, with both of its constants already constant.
///
/// Template parameters rather than arguments because that is the whole
/// requirement - the switch in @c prefetch is what supplies them, and this is
/// the only place the per-compiler difference is spelled out.
template <int Rw, int Locality>
void issue(const void *address) noexcept {
	static_assert(Rw == 0 || Rw == 1);
	static_assert(Locality >= 0 && Locality <= 3);
#if defined(__GNUC__) || defined(__clang__)
	__builtin_prefetch(address, Rw, Locality);
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
	_mm_prefetch(static_cast<const char *>(address),
				 MSVC_PREFETCH_HINT<Locality>);
#else
	// No intrinsic: the hint is dropped, which is exactly what a hint is
	// allowed to be. Discarding the argument keeps the call site warning-free.
	(void)address;
#endif
}
} // namespace detail

/**
 * @brief Hint that the line holding @p address should be brought into cache.
 *
 * @param address What to pull in. Any address is safe, including a null or an
 *        unmapped one: a prefetch never faults and never traps, which is what
 *        lets a walk prefetch one step past the end of a structure without
 *        checking first. @c const @c void* takes any object pointer without a
 *        cast at the call site.
 * @param hint How soon it is wanted. @see locality_hint
 * @param op Whether the line will be read or written. @see operation
 *
 * @note @p hint precedes @p op because it is the one callers vary. The two
 *       combinations worth naming have wrappers below.
 *
 * @note Not @c [[nodiscard]] and not a promise. The hardware is free to ignore
 *       this entirely, and a hint issued too late costs an instruction and buys
 *       nothing - @c benchmark/ is the only way to know which happened.
 */
inline void prefetch(const void *address,
					 locality_hint hint = locality_hint::high,
					 operation op       = operation::read) noexcept {
	if (op == operation::write) switch (hint) {
		case locality_hint::none: detail::issue<1, 0>(address); return;
		case locality_hint::low: detail::issue<1, 1>(address); return;
		case locality_hint::moderate: detail::issue<1, 2>(address); return;
		case locality_hint::high: detail::issue<1, 3>(address); return;
		}
	else switch (hint) {
		case locality_hint::none: detail::issue<0, 0>(address); return;
		case locality_hint::low: detail::issue<0, 1>(address); return;
		case locality_hint::moderate: detail::issue<0, 2>(address); return;
		case locality_hint::high: detail::issue<0, 3>(address); return;
		}
}

/**
 * @brief Hint a line that will be touched once and not wanted again.
 *
 * The streaming case: issues the hint without evicting what the working set is
 * using, so a pass over data larger than the cache does not cost the caller the
 * cache it was already holding. @c prefetchnta on x86-64.
 *
 * @note Named for the reuse the caller knows about, not for a cache level.
 *       @c __builtin_prefetch's locality argument does not select one - 3 lands
 *       on @c prefetcht0, which fetches into *every* level rather than into L1,
 *       and the arm64 lowering shares none of that vocabulary. A
 *       @c prefetch_into_l1 would be naming a placement no ISA here promises.
 */
inline void prefetch_streaming(const void *address) noexcept {
	detail::issue<0, 0>(address);
}

/**
 * @brief Hint a line that is about to be written.
 *
 * Asks for the line in a state the following store will not have to upgrade -
 * warmed for exclusive ownership rather than shared, so the store does not
 * stall on a read-for-ownership.
 *
 * @warning Currently identical to @c prefetch on every configuration this tree
 *          builds. Measured with GCC @c -O2 on x86-64: @c rw=1 and @c rw=0 both
 *          emit @c prefetcht0. The distinct instruction is @c prefetchw, and
 *          the backend only reaches for it under @c -mprfchw - not even
 *          @c -march=x86-64-v3 implies it. MSVC exposes no write form of
 *          @c _mm_prefetch at all.
 *
 *          So this records what the call site knows and costs nothing to say;
 *          it does not currently buy an instruction. Prefer it over @c prefetch
 *          where the intent is a store, and treat any claimed speedup from it
 *          as a guess until @c benchmark/ says otherwise. @see prefetch
 */
inline void prefetch_for_write(const void *address) noexcept {
	detail::issue<1, 3>(address);
}

/**
 * @brief Hint every cache line spanned by @p bytes from @p address.
 *
 * One hint per line rather than per element: a second hint for an address on a
 * line already requested is an instruction that buys nothing.
 *
 * @warning @p bytes is a budget, not a length. Requesting more than the level
 *          being aimed at can hold evicts the front of what was just pulled in,
 *          so a caller warming a large buffer should clamp to the cache it
 *          means to fill, or pass @c locality_hint::none for a pass that is
 *          read once and should not disturb the working set. The size of that
 *          cache is a runtime property of the host -
 *          @c concurrency::last_level_cache_size reports it.
 */
inline void prefetch_range(const void *address, std::size_t bytes,
						   locality_hint hint = locality_hint::high,
						   operation op       = operation::read) noexcept {
	const auto *const first = static_cast<const std::byte *>(address);
	for (std::size_t offset = 0; offset < bytes;
		 offset += concurrency::CACHE_LINE_SIZE)
		prefetch(first + offset, hint, op);
}

} // namespace exchange::core::optimisation
