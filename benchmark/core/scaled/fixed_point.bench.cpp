#include "core/scaled/fixed_point.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <optional>
#include <random>
#include <string_view>
#include <utility>
#include <vector>

// The parser's hot path is decoding decimal price/size strings off the feed.
// These benchmarks feed it a reproducible mix of widths so the SWAR eight-digit
// fold and the SSE sixteen-digit fold are both exercised, then measure the
// per-call cost and throughput. RNG and vector build happen outside the timed
// region; the returned value is folded into a checksum to defeat dead-code
// elimination.
namespace {
// A pure per-character scalar parser with the same grammar as
// parse_fixed_point, kept here purely as a benchmark baseline: it quantifies
// what the SWAR/SSE digit folds buy over a naive byte-at-a-time loop. It is not
// part of the shipped parser - do not use it elsewhere.
namespace scalar_baseline {
[[nodiscard]] constexpr bool mul_add(std::int64_t &v, std::int64_t m,
									 std::int64_t a) noexcept {
	if (v > (std::numeric_limits<int64_t>::max() - a) / m) return false;
	v = v * m + a;
	return true;
}

[[nodiscard]] std::optional<std::int64_t> parse(std::string_view t,
												int scale) noexcept {
	if (scale < 0 || t.empty()) return std::nullopt;
	const char *p         = t.data();
	const char *const end = p + t.size();
	bool negative         = false;
	if (*p == '+' || *p == '-') {
		negative = *p == '-';
		++p;
	}
	std::int64_t value = 0;
	bool any_digit     = false;
	int consumed       = 0;
	while (p != end && isdigit(*p)) {
		if (!mul_add(value, 10, *p - '0')) return std::nullopt;
		++p;
		any_digit = true;
	}
	if (p != end && *p == '.') {
		++p;
		while (consumed < scale && p != end && isdigit(*p)) {
			if (!mul_add(value, 10, *p - '0')) return std::nullopt;
			++p;
			++consumed;
			any_digit = true;
		}
		while (p != end && isdigit(*p)) ++p;
	}
	if (p != end || !any_digit) return std::nullopt;
	for (; consumed < scale; ++consumed)
		if (!mul_add(value, 10, 0)) return std::nullopt;
	return negative ? -value : value;
}
} // namespace scalar_baseline

// A reproducible corpus of realistic exchange decimals, laid out as ONE
// contiguous byte buffer with a parallel vector of string_views into it - no
// std::string on the parse path. That is deliberate: on the real feed the
// parser reads each decimal straight out of the JSON parser's buffer, so the
// benchmark must feed it the same shape. A std::vector<std::string> would
// scatter every value into its own heap block (the 17-char samples exceed the
// small-string buffer), turning the timed loop into a pointer-chase that
// measures the allocator and cache instead of the parser.
//
// The width mix drives the fold widths: "12345678.12345678" the 16-digit SSE
// path, "153.45000000" the 8-digit SWAR path, "42.5" the scalar tail.
struct Corpus {
	std::vector<char> bytes;             ///< contiguous storage for every value
	std::vector<std::string_view> views; ///< one view per value, into bytes
};

Corpus corpus(std::size_t n) {
	static constexpr std::string_view samples[] = {
		"153.45000000",      // typical price, scale-8 fraction
		"0.00010000",        // small quantity
		"12345678.12345678", // 16 fractional + 8 integer digits
		"99999999.99999999", // near-max field width
		"42.5",              // short scalar tail
	};
	std::mt19937_64 rng(0xC0'FFEE);
	std::uniform_int_distribution<std::size_t> pick(0, std::size(samples) - 1);

	// Fill the byte buffer first (recording each value's span); build the views
	// only once bytes.data() is final, so no view dangles across a
	// reallocation.
	Corpus c;
	std::vector<std::pair<std::size_t, std::size_t>> spans;
	spans.reserve(n);
	for (std::size_t i = 0; i < n; ++i) {
		const std::string_view s = samples[pick(rng)];
		spans.emplace_back(c.bytes.size(), s.size());
		c.bytes.append_range(s);
	}
	c.views.reserve(n);
	for (const auto &[offset, len] : spans)
		c.views.emplace_back(c.bytes.data() + offset, len);
	return c;
}

// Parse every value in a fixed corpus at scale 8 (Binance's price/qty scale).
// Reports both calls/s (ItemsProcessed) and MB/s (BytesProcessed).
void BM_ParseFixedPoint(benchmark::State &state) {
	using exchange::core::scaled::parse_fixed_point;

	constexpr int scale = 8;
	const Corpus data   = corpus(static_cast<std::size_t>(state.range(0)));

	std::int64_t checksum = 0;
	for (auto _ : state) {
		for (const std::string_view s : data.views) {
			auto parsed = parse_fixed_point(s, scale);
			if (parsed) checksum += *parsed;
			benchmark::DoNotOptimize(checksum);
		}
	}
	benchmark::DoNotOptimize(checksum);

	state.SetItemsProcessed(state.iterations() *
							static_cast<std::int64_t>(data.views.size()));
	state.SetBytesProcessed(state.iterations() *
							static_cast<std::int64_t>(data.bytes.size()));
}

BENCHMARK(BM_ParseFixedPoint)
->RangeMultiplier(8)->Range(64, 64 << 10);

// Same corpus, same scale, through the naive scalar loop - the SIMD-free
// baseline. Compare its ns/call against BM_ParseFixedPoint to read off the
// speed-up the digit folds deliver on this width mix.
void BM_ParseFixedPoint_Scalar(benchmark::State &state) {
	constexpr int scale = 8;
	const Corpus data   = corpus(static_cast<std::size_t>(state.range(0)));

	std::int64_t checksum = 0;
	for (auto _ : state) {
		for (const std::string_view s : data.views) {
			auto parsed = scalar_baseline::parse(s, scale);
			if (parsed) checksum += *parsed;
			benchmark::DoNotOptimize(checksum);
		}
	}
	benchmark::DoNotOptimize(checksum);

	state.SetItemsProcessed(state.iterations() *
							static_cast<std::int64_t>(data.views.size()));
	state.SetBytesProcessed(state.iterations() *
							static_cast<std::int64_t>(data.bytes.size()));
}

BENCHMARK(BM_ParseFixedPoint_Scalar)
->RangeMultiplier(8)->Range(64, 64 << 10);
} // namespace