#pragma once

// fmt formatters for the market-data composite value types.
//
// Opt-in, like fmt's own fmt/std.h and fmt/ranges.h: only translation units
// that actually print one of these pay for <fmt/format.h>, so the decoder and
// book headers stay free of it. A missing include is a compile error, never a
// silently different rendering.

#include "binance/binance_depth.hpp"
#include "binance/endpoints.hpp"
#include "l2_book.hpp"
#include "normalised.hpp"
#include "sequencer.hpp"

#include <fmt/format.h>

#include <string_view>

/// @brief An aggregated level as @c "@15000 x 100" (price, absolute size).
template <>
struct fmt::formatter<exchange::market_data::l2_book::Level>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::market_data::l2_book::Level &level,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out, "@{} x {}", level.price, level.qty);
		});
	}
};

/// @brief A decoded wire level as @c "@15000 x 100". Same shape as an
///        l2_book::Level — a diff level drops straight into the book.
template <>
struct fmt::formatter<exchange::market_data::binance::PriceLevel>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::market_data::binance::PriceLevel &level,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out, "@{} x {}", level.price, level.qty);
		});
	}
};

/// @brief A reconstructed book as
///        @c "l2_book[bids=3 asks=2 best @15000 x 7 / @15001 x 4]" — depth and
///        top of book, the two things a log line actually wants. Sides with no
///        levels read @c "none".
template <>
struct fmt::formatter<exchange::market_data::l2_book>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::market_data::l2_book &book,
				format_context &ctx) const -> format_context::iterator {
		using exchange::side;
		const auto &bids = book.levels(side::bid);
		const auto &asks = book.levels(side::ask);
		return write_padded(ctx, [&](auto out) {
			out = fmt::format_to(out,
								 "l2_book[bids={} asks={} best ",
								 bids.size(),
								 asks.size());
			if (bids.empty()) out = fmt::format_to(out, "none");
			else out = fmt::format_to(out, "{}", bids.front());
			out = fmt::format_to(out, " / ");
			if (asks.empty()) out = fmt::format_to(out, "none");
			else out = fmt::format_to(out, "{}", asks.front());
			return fmt::format_to(out, "]");
		});
	}
};

/// @brief A depth-parse failure as @c "[line L: ][context: ]category" — the
///        same text @c binance::message() returns, which is now defined in
///        terms of this so the two cannot drift.
template <>
struct fmt::formatter<exchange::market_data::binance::depth_parse_error>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::market_data::binance::depth_parse_error &error,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			if (error.line) out = fmt::format_to(out, "line {}: ", error.line);
			if (!error.context.empty())
				out = fmt::format_to(out, "{}: ", error.context);
			// error.code goes through its format_as -> category message.
			return fmt::format_to(out, "{}", error.code);
		});
	}
};

/// @brief A WebSocket endpoint as the @c wss:// URL it denotes — paste-able
///        straight into a client when a capture misbehaves.
template <>
struct fmt::formatter<exchange::market_data::binance::stream_endpoint>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::market_data::binance::stream_endpoint &endpoint,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out,
								  "wss://{}:{}{}",
								  endpoint.host,
								  endpoint.port,
								  endpoint.target);
		});
	}
};

/// @brief A REST endpoint as the @c https:// URL it denotes.
template <>
struct fmt::formatter<exchange::market_data::binance::http_endpoint>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::market_data::binance::http_endpoint &endpoint,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out,
								  "https://{}{}",
								  endpoint.host,
								  endpoint.target);
		});
	}
};

/// @brief A REST snapshot as @c "DepthSnapshot[lastUpdateId=1 bids=100
///        asks=100]" — its sequencing id and shape, not its levels.
template <>
struct fmt::formatter<exchange::market_data::binance::DepthSnapshot>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::market_data::binance::DepthSnapshot &snapshot,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out,
								  "DepthSnapshot[lastUpdateId={} bids={} "
								  "asks={}]",
								  snapshot.lastUpdateId,
								  snapshot.bids.size(),
								  snapshot.asks.size());
		});
	}
};

/// @brief A diff event as @c "depthUpdate[U=1 u=5 bids=3 asks=2]", naming the
///        update-id bounds the way Binance's own field letters do — those are
///        what you compare against lastUpdateId to sequence the local book.
template <>
struct fmt::formatter<exchange::market_data::binance::DepthUpdate>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::market_data::binance::DepthUpdate &update,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out,
								  "depthUpdate[U={} u={} bids={} asks={}]",
								  update.firstUpdateId,
								  update.finalUpdateId,
								  update.bids.size(),
								  update.asks.size());
		});
	}
};

/// @brief The bookkeeping half of a diff event, as @c "depthUpdate[U=1 u=5]".
template <>
struct fmt::formatter<exchange::market_data::binance::DepthUpdateMeta>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::market_data::binance::DepthUpdateMeta &meta,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out,
								  "depthUpdate[U={} u={}]",
								  meta.firstUpdateId,
								  meta.finalUpdateId);
		});
	}
};

/// @brief A sequence span as @c "1..5", or just @c "5" when it covers a single
///        number — the range notation reads the same for every venue, which is
///        the point of normalising away @c U / @c u.
template <>
struct fmt::formatter<exchange::market_data::sequence_range>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::market_data::sequence_range &sequence,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			if (sequence.first == sequence.last)
				return fmt::format_to(out, "{}", sequence.first);
			return fmt::format_to(out,
								  "{}..{}",
								  sequence.first,
								  sequence.last);
		});
	}
};

/// @brief A normalised diff as @c "depth_event[1..5 bids=3 asks=2]".
template <>
struct fmt::formatter<exchange::market_data::depth_event>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::market_data::depth_event &event,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out,
								  "depth_event[{} bids={} asks={}]",
								  event.sequence,
								  event.bids.size(),
								  event.asks.size());
		});
	}
};

/// @brief A normalised snapshot as @c "book_snapshot[seq=42 bids=100
///        asks=100]" — the sequence it seeds from and its shape.
template <>
struct fmt::formatter<exchange::market_data::book_snapshot>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::market_data::book_snapshot &snapshot,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out,
								  "book_snapshot[seq={} bids={} asks={}]",
								  snapshot.sequence,
								  snapshot.bids.size(),
								  snapshot.asks.size());
		});
	}
};

/// @brief Feed health as @c "seq[applied=5 discarded=1 buffered=2 overlapped=0
///        gaps=1]" — one log line that says whether the replica can be trusted.
template <>
struct fmt::formatter<exchange::market_data::sequencer_stats>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::market_data::sequencer_stats &stats,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out,
								  "seq[applied={} discarded={} buffered={} "
								  "overlapped={} gaps={}]",
								  stats.applied,
								  stats.discarded,
								  stats.buffered,
								  stats.overlapped,
								  stats.gaps);
		});
	}
};
