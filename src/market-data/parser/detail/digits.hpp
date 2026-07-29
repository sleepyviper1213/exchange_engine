#pragma once
// Private, reusable SIMD digit primitives for the parser module. These fold or
// validate a fixed run of ASCII digits with no branches per byte; the parsing
// logic that stitches sign, decimal point and truncation around them lives in
// the module's .cpp files. Not part of the public API — include only from
// within the parser module.
#include <bit>
#include <cstdint>
#include <cstring>

// The SSE variants compile only when the target guarantees SSE4.1 at build time
// (e.g. -msse4.1 / -march=native), so no runtime CPU dispatch is needed: where
// they are absent the portable SWAR path below covers every case identically.
// MSVC's x64 baseline is SSE2, so it uses SWAR unless built for a wider ISA.
#if defined(__SSE4_1__)
#include <immintrin.h>
#define PARSER_DETAIL_HAS_SSE41 1
#endif

namespace exchange::market_data::parser::detail {

/// @brief True on targets whose byte order matches the SWAR fold below.
inline constexpr bool SWAR_NATIVE =
	std::endian::native == std::endian::little;

/**
 * @brief Are all eight bytes of @p word ASCII digits ('0'..'9')?
 *
 * Endian-independent: the test is applied per byte. @p word is a raw
 * little-endian load of eight characters.
 */
[[nodiscard]] constexpr bool is_eight_digits(std::uint64_t word) noexcept {
	return ((word & 0xF0F0F0F0F0F0F0F0ULL) |
			(((word + 0x0606060606060606ULL) & 0xF0F0F0F0F0F0F0F0ULL) >> 4)) ==
		   0x3333333333333333ULL;
}

/**
 * @brief Fold eight validated ASCII digits into their numeric value.
 *
 * Classic three-step SWAR reduction: pair adjacent digits, then quads, then the
 * two halves.
 * @param word A little-endian load of eight ASCII digits.
 * @pre Every byte of @p word is a digit (see @c is_eight_digits) and the
 *      platform is little-endian (@c SWAR_NATIVE).
 * @return The eight digits read most-significant first, e.g. "12345678" -> 12345678.
 */
[[nodiscard]] constexpr std::uint32_t
parse_eight_digits(std::uint64_t word) noexcept {
	word = (word & 0x0F0F0F0F0F0F0F0FULL) * 2561 >> 8;
	word = (word & 0x00FF00FF00FF00FFULL) * 6553601 >> 16;
	return static_cast<std::uint32_t>((word & 0x0000FFFF0000FFFFULL) *
										  42949672960001ULL >>
									  32);
}

#ifdef PARSER_DETAIL_HAS_SSE41
/**
 * @brief Are all sixteen bytes at @p p ASCII digits?
 * @pre @p p addresses at least sixteen readable bytes.
 */
[[nodiscard]] inline bool is_sixteen_digits(const char *p) noexcept {
	const __m128i ascii = _mm_loadu_si128(reinterpret_cast<const __m128i *>(p));
	const __m128i shifted = _mm_sub_epi8(ascii, _mm_set1_epi8('0'));
	// Unsigned "> 9": saturating-subtract 9; a digit yields 0, anything else >0.
	const __m128i over = _mm_subs_epu8(shifted, _mm_set1_epi8(9));
	return _mm_movemask_epi8(_mm_cmpeq_epi8(over, _mm_setzero_si128())) == 0xFFFF;
}

/**
 * @brief Fold sixteen validated ASCII digits at @p p into their value.
 * @pre @p p addresses sixteen readable ASCII digits (see @c is_sixteen_digits).
 */
[[nodiscard]] inline std::uint64_t parse_sixteen_digits(const char *p) noexcept {
	const __m128i ascii  = _mm_loadu_si128(reinterpret_cast<const __m128i *>(p));
	const __m128i digits = _mm_sub_epi8(ascii, _mm_set1_epi8('0'));
	const __m128i mul_1_10 =
		_mm_setr_epi8(10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1);
	const __m128i pairs = _mm_maddubs_epi16(digits, mul_1_10);   // 8 x 0..99
	const __m128i mul_1_100 = _mm_setr_epi16(100, 1, 100, 1, 100, 1, 100, 1);
	const __m128i quads  = _mm_madd_epi16(pairs, mul_1_100);     // 4 x 0..9999
	const __m128i packed = _mm_packus_epi32(quads, quads);       // -> uint16 lanes
	const __m128i mul_1_10000 =
		_mm_setr_epi16(10000, 1, 10000, 1, 0, 0, 0, 0);
	const __m128i halves = _mm_madd_epi16(packed, mul_1_10000);  // 2 x 8-digit
	const std::uint64_t hi =
		static_cast<std::uint32_t>(_mm_cvtsi128_si32(halves));
	const std::uint64_t lo =
		static_cast<std::uint32_t>(_mm_extract_epi32(halves, 1));
	return hi * 100000000ULL + lo;
}
#endif

} // namespace exchange::market_data::parser::detail
