#include "seed.hpp"

#include "core/logging.hpp"
#include "core/util/slurp.hpp"
#include "market_data/binance/binance_depth.hpp"
#include "market_data/format.hpp" // IWYU pragma: keep - depth_parse_error

#include <spdlog/spdlog.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace exchange::app {
namespace {

namespace binance = exchange::market_data::binance;

/// The file's first non-blank line, trimmed of a trailing carriage return.
std::string_view first_line(std::string_view text) noexcept {
	while (!text.empty()) {
		const std::size_t eol = text.find('\n');
		std::string_view line = text.substr(
			0,
			eol == std::string_view::npos ? std::string_view::npos : eol);
		if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
		if (!line.empty() &&
			line.find_first_not_of(" \t") != std::string_view::npos)
			return line;
		if (eol == std::string_view::npos) break;
		text.remove_prefix(eol + 1);
	}
	return {};
}

/// Whether @p text is a diff capture rather than a snapshot.
///
/// Decided by running the real depthUpdate decoder over the first line, not by
/// looking for `"e":"depthUpdate"` in the bytes. A substring test would call a
/// snapshot that happens to contain that text a capture, and would miss a
/// capture whose frames arrived without the `e` field at all.
bool is_diff_capture(std::string_view text, int price_decimals,
					 int qty_decimals) {
	const std::string_view line = first_line(text);
	if (line.empty()) return false;
	return binance::parse_binance_depth_update(line,
											   price_decimals,
											   qty_decimals)
		.has_value();
}

} // namespace

std::optional<binance::depth_snapshot>
load_seed_snapshot(const std::string &path, int price_decimals,
				   int qty_decimals) {
	const std::string json = exchange::core::util::slurp(path);
	if (json.empty()) {
		spdlog::error("cannot read {} (missing or empty)", path);
		return std::nullopt;
	}

	auto snapshot =
		binance::parse_binance_depth(json, price_decimals, qty_decimals);
	if (snapshot) return std::move(*snapshot);

	// The mistake worth naming, before the parse error nobody can act on.
	if (is_diff_capture(json, price_decimals, qty_decimals))
		spdlog::error(
			"{} is a diff capture, not a REST depth snapshot: its frames are "
			"depthUpdate objects with b/a level arrays, and a snapshot is one "
			"object with lastUpdateId and bids/asks. Fetch one with: curl -s "
			"'https://api.binance.com/api/v3/depth?symbol=SYMBOL&limit=1000' "
			"-o snapshot.json - while the capture is still running, or every "
			"frame in it will be discarded as already covered",
			path);
	else
		spdlog::error("snapshot parse failed for {}: {}",
					  path,
					  snapshot.error());
	return std::nullopt;
}

} // namespace exchange::app
