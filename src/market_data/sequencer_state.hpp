#pragma once
// The two enums the sequencer speaks in: what to do with an event, and whether
// the local book is a live replica.
//
// Split from sequencer.hpp because they are the *answers* the sequencer gives,
// and a caller that only switches on one - a reconstructor, a feed-health
// dashboard, a formatter - has no business also reading the state machine that
// produces them. Each prints, and reads through magic_enum::enum_name, as the
// description specialised at the foot of this file.

#include "core/util/enum_format.hpp"
#include "fwd.hpp"

#include <cstdint>

namespace exchange::market_data {

using core::util::format_as;

/// @brief What the sequencer says to do with an event.
enum class sequence_action : std::uint8_t { buffer, discard, apply, gap };

/// @brief Whether the local book is a live replica.
enum class sync_state : std::uint8_t { awaiting_snapshot, streaming };

} // namespace exchange::market_data

template <>
constexpr magic_enum::customize::customize_t magic_enum::customize::enum_name(
	exchange::market_data::sequence_action value) noexcept {
	switch (value) {
		using enum exchange::market_data::sequence_action;
	case buffer: return "no snapshot yet; retain the event";
	case discard: return "already covered by the snapshot; drop it";
	case apply: return "resumes the sequence; apply it to the book";
	case gap: return "sequence discontinuity; the book is stale";
	}
	return default_tag;
}

template <>
constexpr magic_enum::customize::customize_t magic_enum::customize::enum_name(
	exchange::market_data::sync_state value) noexcept {
	switch (value) {
		using enum exchange::market_data::sync_state;
	case awaiting_snapshot: return "unsynced; events must be buffered";
	case streaming: return "seeded and in sequence; events apply directly";
	}
	return default_tag;
}
