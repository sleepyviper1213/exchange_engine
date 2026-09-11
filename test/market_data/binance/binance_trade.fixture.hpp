#pragma once
// The wire payload the trade-decoder suites share: one `@trade` frame, shaped
// exactly as Binance sends one, in wire order (e, E, s, t, p, q, T, m, M).
//
// The order is the point. simdjson On-Demand walks a document forwards and does
// not rewind, so a decoder that reads the fields in any other order works only
// by accident - and a fixture that listed them alphabetically would let that
// bug through.

#include <string>
#include <string_view>

/// @brief A `@trade` frame: SOLUSDT, 10.00 lots at 153.45, buyer as maker.
constexpr std::string_view BINANCE_TRADE_JSON =
	R"({"e":"trade","E":1571889248277,"s":"SOLUSDT","t":390497796,)"
	R"("p":"153.45","q":"10.00","T":1571889248270,"m":true,"M":true})";

/// @brief The scales @c BINANCE_TRADE_JSON is written for.
constexpr int BINANCE_TRADE_PRICE_DECIMALS = 2;
constexpr int BINANCE_TRADE_QTY_DECIMALS   = 2;

/// @brief What those scales make of the fixture's price and size.
constexpr long long BINANCE_TRADE_PRICE = 15345;
constexpr long long BINANCE_TRADE_QTY   = 1000;

/// @brief The fixture frame's trade id, execution time and send time.
constexpr unsigned long long BINANCE_TRADE_ID      = 390'497'796;
constexpr unsigned long long BINANCE_TRADE_SENT_MS = 1'571'889'248'277;
constexpr unsigned long long BINANCE_TRADE_EXEC_MS = 1'571'889'248'270;

/// @brief A tape of @p frames prints with consecutive trade ids.
///
/// Consecutive because that is what the venue guarantees on one symbol, and
/// because a tape's only continuity check is arithmetic on those ids - a corpus
/// that repeated one id would make a gap test pass for the wrong reason.
inline std::string
binance_trade_tape(int frames, unsigned long long first_id = BINANCE_TRADE_ID) {
	std::string jsonl;
	for (int i = 0; i < frames; ++i) {
		jsonl.append(R"({"e":"trade","E":1571889248277,"s":"SOLUSDT","t":)");
		jsonl.append(std::to_string(first_id + static_cast<unsigned>(i)));
		jsonl.append(R"(,"p":"153.45","q":"10.00","T":1571889248270,)");
		jsonl.append(R"("m":true,"M":true})");
		jsonl.push_back('\n');
	}
	return jsonl;
}
