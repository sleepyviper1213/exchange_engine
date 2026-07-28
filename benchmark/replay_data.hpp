#pragma once

#include "market-data/binance/binance_depth.hpp"
#include "market-data/l2_book.hpp"
#include "trading-engine/order_book/order_book.hpp"
#include "core/util/slurp.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Offline replay input for market_replay.cpp: a seed book plus a diff-depth
// feed, materialized once outside the timed region so the benchmark measures
// matching, not network or JSON cost.
//
// Point OB_REPLAY at a JSONL capture of depthUpdate frames (one per line, e.g.
// from ws_capture_tool); otherwise a representative stream is synthesized.
// Point OB_SNAPSHOT at a saved REST depth JSON for the seed book; otherwise a
// SOLUSDT- shaped book is synthesized. OB_PRICE_DECIMALS / OB_QTY_DECIMALS
// override the tick/step precision used to parse both files (default 2,
// matching SOLUSDT).
namespace replay {

using exchange::Price;
using exchange::Side;
using exchange::Volume;
using exchange::engine::order_book;
using exchange::market_data::l2_book;
namespace binance = exchange::market_data::binance;
using exchange::core::util::slurp;

// SOLUSDT-shaped synthetic defaults: mid ~150.00, 0.01 tick, 2 decimals.
constexpr int kDefaultDecimals     = 2;
constexpr Price kSynthMid          = 15000; // 150.00 scaled by 10^2
constexpr std::size_t kSynthDepth  = 1000;  // levels per side in the seed book
constexpr std::size_t kSynthEvents = 5000;  // diff events in the synthetic feed
constexpr std::size_t kSynthTouchPerSide =
	12;                                     // levels touched per side per event
constexpr std::size_t kSynthWindow =
	200; // ticks around top-of-book a diff hits

/**
 * @brief The offline replay input, materialized once outside the timed region.
 */
struct ReplayData {
	binance::DepthSnapshot snap;            ///< seed book
	std::vector<binance::DepthUpdate> feed; ///< diff events to replay
	std::size_t levels = 0;                 ///< total touched levels (items)
};

/**
 * @brief Read an integer environment variable, falling back on absence.
 * @param name Environment variable name.
 * @param fallback Value returned when @p name is unset.
 * @return The parsed integer, or @p fallback if the variable is not set.
 */
inline int env_int(const char *name, int fallback) {
	if (const char *raw = std::getenv(name)) return std::atoi(raw);
	return fallback;
}

/**
 * @brief Obtain the seed order book snapshot.
 * @param price_decimals Tick precision used to parse an OB_SNAPSHOT file.
 * @param qty_decimals Step precision used to parse an OB_SNAPSHOT file.
 * @return The snapshot parsed from OB_SNAPSHOT, or a synthesized
 *         SOLUSDT-shaped book when the variable is unset.
 * @note Aborts if an OB_SNAPSHOT file is set but fails to parse.
 */
inline binance::DepthSnapshot snapshot(int price_decimals, int qty_decimals) {
	if (const char *path = std::getenv("OB_SNAPSHOT")) {
		auto parsed = binance::parse_binance_depth(slurp(path),
												   price_decimals,
												   qty_decimals);
		if (!parsed) std::abort();
		return *parsed;
	}
	binance::DepthSnapshot s;
	s.bids.reserve(kSynthDepth);
	s.asks.reserve(kSynthDepth);
	for (std::size_t i = 0; i < kSynthDepth; ++i) {
		const auto tick = static_cast<Price>(i);
		s.bids.emplace_back(kSynthMid - tick, 100);
		s.asks.emplace_back(kSynthMid + 1 + tick, 100);
	}
	return s;
}

/**
 * @brief Synthesize a representative diff-depth stream from a seed book.
 *
 * Each event re-sizes a small window of near-touch levels, occasionally to 0
 * (a cancel/removal), mirroring how a live @c \@depth feed churns the top of
 * book. Deterministic: a fixed RNG seed keeps the timed region reproducible.
 * @param seed The book the stream perturbs (top of book anchors the window).
 * @return The synthesized diff events.
 */
inline std::vector<binance::DepthUpdate>
synth_updates(const binance::DepthSnapshot &seed) {
	const Price best_bid =
		seed.bids.empty() ? kSynthMid : seed.bids.front().price;
	const Price best_ask =
		seed.asks.empty() ? kSynthMid + 1 : seed.asks.front().price;

	std::mt19937_64 rng(1'234'567);
	std::uniform_int_distribution<Price> off(0, kSynthWindow);
	std::uniform_int_distribution<Volume> qty(0, 200); // 0 ~ removal
	std::uniform_int_distribution<int> drift(-2, 2);

	std::vector<binance::DepthUpdate> updates;
	updates.reserve(kSynthEvents);
	Price bid_ref           = best_bid;
	Price ask_ref           = best_ask;
	std::uint64_t update_id = 1;
	for (std::size_t e = 0; e < kSynthEvents; ++e) {
		binance::DepthUpdate u;
		u.firstUpdateId = update_id;
		for (std::size_t k = 0; k < kSynthTouchPerSide; ++k) {
			u.bids.push_back({bid_ref - off(rng), qty(rng)});
			u.asks.push_back({ask_ref + off(rng), qty(rng)});
		}
		update_id += u.bids.size() + u.asks.size();
		u.finalUpdateId = update_id - 1;
		updates.push_back(std::move(u));

		// Wander the reference prices a little so the touched window moves.
		bid_ref =
			static_cast<Price>(static_cast<std::int64_t>(bid_ref) + drift(rng));
		ask_ref =
			static_cast<Price>(static_cast<std::int64_t>(ask_ref) + drift(rng));
	}
	return updates;
}

/**
 * @brief Obtain the diff-depth feed to replay.
 * @param seed Seed book used when synthesizing (ignored for OB_REPLAY files).
 * @param price_decimals Tick precision used to parse an OB_REPLAY file.
 * @param qty_decimals Step precision used to parse an OB_REPLAY file.
 * @return The events parsed from OB_REPLAY, or a synthesized stream when the
 *         variable is unset.
 * @note Aborts if an OB_REPLAY file is set but fails to parse.
 */
inline std::vector<binance::DepthUpdate>
updates(const binance::DepthSnapshot &seed, int price_decimals,
		int qty_decimals) {
	if (const char *path = std::getenv("OB_REPLAY")) {
		auto parsed = binance::parse_binance_depth_updates(
			slurp(path),
			price_decimals,

			qty_decimals);
		if (!parsed) std::abort();
		return *parsed;
	}
	return synth_updates(seed);
}

/**
 * @brief Seed @p book with a snapshot's levels via absolute set_level updates.
 * @param book Book to populate (assumed empty).
 * @param snap Snapshot whose bid/ask levels are inserted.
 */
inline void seed_book(order_book &book, const binance::DepthSnapshot &snap) {
	for (const auto &[price, volume] : snap.bids)
		book.set_level(Side::BID, price, volume);
	for (const auto &[price, volume] : snap.asks)
		book.set_level(Side::ASK, price, volume);
}

/// @brief Seed a cache-optimised l2_book from a snapshot (same set_level
///        semantics as seed_book, for the A/B replay benchmarks).
inline void seed_l2(l2_book &book, const binance::DepthSnapshot &snap) {
	for (const auto &[price, volume] : snap.bids)
		book.set_level(Side::BID, price, volume);
	for (const auto &[price, volume] : snap.asks)
		book.set_level(Side::ASK, price, volume);
}

/// @brief Apply one diff event's absolute levels to an l2_book — the same work
///        binance::apply_depth_update does, spelled out here so the two sides of
///        the A/B run identical code around the book under test.
inline void apply_l2(l2_book &book, const binance::DepthUpdate &update) {
	for (const auto &[price, volume] : update.bids)
		book.set_level(Side::BID, price, volume);
	for (const auto &[price, volume] : update.asks)
		book.set_level(Side::ASK, price, volume);
}

/**
 * @brief Apply one diff event to an order_book — the A/B baseline only.
 *
 * market-data deliberately offers no such function: a diff feed carries no
 * order identity, so pointing the decoder at an order-by-order book is the
 * conflation the subsystem split exists to prevent. The benchmark still needs
 * to measure what that conflation would cost, so it does the mapping itself,
 * here, where it is visibly a measurement fixture and not an entry point.
 */
inline void apply_ob(order_book &book, const binance::DepthUpdate &update) {
	for (const auto &[price, volume] : update.bids)
		book.set_level(Side::BID, price, volume);
	for (const auto &[price, volume] : update.asks)
		book.set_level(Side::ASK, price, volume);
}

/**
 * @brief Load the seed book and diff feed and count the feed's total levels.
 * @return A ReplayData with snapshot, feed, and touched-level count populated.
 */
inline ReplayData load() {
	const int pd = env_int("OB_PRICE_DECIMALS", kDefaultDecimals);
	const int qd = env_int("OB_QTY_DECIMALS", kDefaultDecimals);
	ReplayData data{snapshot(pd, qd), updates(data.snap, pd, qd)};
	for (const auto &u : data.feed)
		data.levels += u.bids.size() + u.asks.size();
	return data;
}

// --- raw-JSON replay: reconstruct the wire bytes for the parse benchmarks ----
//
// The feed above is pre-parsed structs; to benchmark the *parser* we need the
// original depthUpdate JSON back. Each event is serialized to compact Binance
// wire form into ONE contiguous buffer with a string_view per frame — no
// std::string on what the parser reads. Serializing with the same decimals the
// parser uses is an exact round-trip, so the reparsed book matches the struct
// feed level-for-level.

/// @brief A contiguous buffer of depthUpdate JSON frames plus a view per frame.
struct RawFeed {
	std::vector<char> bytes;              ///< every frame's JSON, back to back
	std::vector<std::string_view> frames; ///< one view per frame, into bytes
};

/// @brief The raw-JSON replay input: seed book, wire frames, level count, and
///        the decimals the frames must be parsed with.
struct ReplayRaw {
	binance::DepthSnapshot snap;
	RawFeed feed;
	std::size_t levels = 0;
	int price_decimals = kDefaultDecimals;
	int qty_decimals   = kDefaultDecimals;
};

/// @brief Append @p v to @p out as base-10 ASCII digits.
inline void append_uint(std::vector<char> &out, std::uint64_t v) {
	char tmp[20];
	int n = 0;
	if (v == 0) tmp[n++] = '0';
	else
		while (v) {
			tmp[n++] = static_cast<char>('0' + v % 10);
			v /= 10;
		}
	for (int i = n - 1; i >= 0; --i) out.push_back(tmp[i]);
}

/// @brief Append @p scaled as a fixed-point decimal with @p decimals fraction
///        digits — the inverse of parse_scaled, so it re-parses to @p scaled.
inline void append_decimal(std::vector<char> &out, std::int64_t scaled,
						   int decimals) {
	if (scaled < 0) {
		out.push_back('-');
		scaled = -scaled;
	}
	std::int64_t scale = 1;
	for (int i = 0; i < decimals; ++i) scale *= 10;
	append_uint(
		out,
		static_cast<std::uint64_t>(decimals > 0 ? scaled / scale : scaled));
	if (decimals > 0) {
		out.push_back('.');
		std::int64_t frac = scaled % scale;
		char fb[20];
		for (int i = decimals - 1; i >= 0; --i) {
			fb[i] = static_cast<char>('0' + frac % 10);
			frac /= 10;
		}
		out.insert(out.end(), fb, fb + decimals);
	}
}

/// @brief Serialize one DepthUpdate to compact Binance depthUpdate JSON.
inline void serialize_update(std::vector<char> &out,
							 const binance::DepthUpdate &u, int price_decimals,
							 int qty_decimals) {
	const auto raw = [&](std::string_view s) {
		out.insert(out.end(), s.begin(), s.end());
	};
	const auto levels = [&](const std::vector<binance::PriceLevel> &ls) {
		out.push_back('[');
		for (std::size_t i = 0; i < ls.size(); ++i) {
			if (i) out.push_back(',');
			raw(R"([")");
			append_decimal(out,
						   static_cast<std::int64_t>(ls[i].price),
						   price_decimals);
			raw(R"(",")");
			append_decimal(out,
						   static_cast<std::int64_t>(ls[i].volume),
						   qty_decimals);
			raw(R"("])");
		}
		out.push_back(']');
	};
	raw(R"({"E":)");
	append_uint(out, u.eventTime);
	raw(R"(,"U":)");
	append_uint(out, u.firstUpdateId);
	raw(R"(,"u":)");
	append_uint(out, u.finalUpdateId);
	raw(R"(,"b":)");
	levels(u.bids);
	raw(R"(,"a":)");
	levels(u.asks);
	out.push_back('}');
}

/**
 * @brief Load the seed book and the diff feed as raw depthUpdate JSON frames.
 *
 * Reuses the same snapshot/feed sources as load() (synthetic, or OB_* files),
 * then serializes the feed back to wire JSON so the parse benchmarks can drive
 * a real parser. Materialized once, outside the timed region.
 */
inline ReplayRaw load_raw() {
	const int pd = env_int("OB_PRICE_DECIMALS", kDefaultDecimals);
	const int qd = env_int("OB_QTY_DECIMALS", kDefaultDecimals);
	ReplayRaw data;
	data.price_decimals = pd;
	data.qty_decimals   = qd;
	data.snap           = snapshot(pd, qd);
	const auto feed     = updates(data.snap, pd, qd);

	// Fill the byte buffer first (recording each frame's span); build the views
	// only once bytes.data() is final, so no view dangles across a
	// reallocation.
	std::vector<std::pair<std::size_t, std::size_t>> spans;
	spans.reserve(feed.size());
	for (const auto &u : feed) {
		const std::size_t off = data.feed.bytes.size();
		serialize_update(data.feed.bytes, u, pd, qd);
		spans.emplace_back(off, data.feed.bytes.size() - off);
		data.levels += u.bids.size() + u.asks.size();
	}
	data.feed.frames.reserve(feed.size());
	for (const auto &[off, len] : spans)
		data.feed.frames.emplace_back(data.feed.bytes.data() + off, len);
	return data;
}
} // namespace replay
