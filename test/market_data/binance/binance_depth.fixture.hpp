#pragma once
// The two wire payloads the depth-decoder suites share: one REST snapshot and
// one depthUpdate frame, both shaped exactly as Binance sends them.

#include <string_view>


/// @brief A /api/v3/depth snapshot: an update id and two levels a side.
constexpr std::string_view SNAPSHOT_JSON =
	R"({"lastUpdateId":123,)"
	R"("bids":[["153.45","10.00"],["153.44","5.50"]],)"
	R"("asks":[["153.46","8.00"],["153.47","2.00"]]})";

/// @brief A depthUpdate frame in wire order: e, E, s, U, u, b, a. The "0.00"
///        bid is a removal, which is what makes this payload worth sharing -
///        every decoder path has to preserve it as an absolute zero rather than
///        dropping the level on the floor.
constexpr std::string_view UPDATE_JSON =
	R"({"e":"depthUpdate","E":1571889248277,"s":"SOLUSDT",)"
	R"("U":390497796,"u":390497878,)"
	R"("b":[["153.45","0.00"],["153.44","5.50"]],)"
	R"("a":[["153.46","8.00"]]})";

