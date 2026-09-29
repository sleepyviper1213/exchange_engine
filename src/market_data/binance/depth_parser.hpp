#pragma once
// Depth parsing for the steady state: one parser, reused frame after frame.
// @see parse_depth.hpp for the one-shot forms this exists to replace on a live
// feed.

#include "core/util/indirect.hpp"
#include "depth_parse_error.hpp"
#include "depth_snapshot.hpp"
#include "depth_update.hpp"
#include "depth_update_meta.hpp"
#include "fwd.hpp"
#include "market_data/l2_book.hpp"
#include "market_data_export.hpp" // MARKET_DATA_EXPORT (generated)

#include <expected>
#include <string_view>

namespace exchange::market_data::binance {

/**
 * @brief A reusable depth parser for the steady-state hot path.
 *
 * The free @c parse_binance_depth* functions construct a fresh simdjson parser
 * and a fresh padded input buffer on every call - fine for one-shot use,
 * wasteful when decoding a @c \@depth WebSocket stream frame after frame. This
 * owns both across calls: simdjson amortizes its internal structural-index/tape
 * buffers, and the input buffer's allocation is reused (only regrown when a
 * frame is larger than any seen so far). On a steady feed this removes the
 * per-frame allocations that dominate tick-to-book latency.
 *
 * @note Stateful and @b not thread-safe - use one instance per consuming
 * thread. Each returned view/snapshot is independent of the parser's buffers
 * (levels are materialised into owned vectors before returning), so results
 * outlive the next @c parse_* call.
 */
class depth_parser {
public:
	MARKET_DATA_EXPORT depth_parser();
	MARKET_DATA_EXPORT ~depth_parser();
	/**
	 * @brief Not copyable, and with @c core::util::indirect that is now a
	 *        decision rather than a consequence.
	 *
	 * A @c unique_ptr pimpl cannot be copied, so the deletion used to be the
	 * language's doing. @c indirect *is* copyable - deep-copying what it owns
	 * is the whole reason C++26 adds it - so a copyable depth_parser is only a
	 * `= default` away, and it is refused because the simdjson parser and its
	 * reused buffers behind
	 * @c impl are not copyable either.
	 */
	depth_parser(const depth_parser &)            = delete;
	depth_parser &operator=(const depth_parser &) = delete;

	/**
	 * @brief Movable, and both halves defined out of line.
	 *
	 * @c impl is incomplete here, and moving out of an @c indirect steals a
	 * pointer while move-*assignment* first destroys what this one owns - which
	 * needs the complete type. So does the destructor. All three are declared
	 * here and defined in the .cpp beside @c impl; a compiler-generated one in
	 * this header would not compile.
	 *
	 * @post The moved-from object owns no @c impl. @see
	 * indirect::valueless_after_move
	 */
	MARKET_DATA_EXPORT depth_parser(depth_parser &&) noexcept;
	MARKET_DATA_EXPORT depth_parser &operator=(depth_parser &&) noexcept;

	/**
	 * @brief Parse a REST depth snapshot, reusing this parser's buffers.
	 * @param json The raw JSON body.
	 * @param priceDecimals Tick precision for the symbol.
	 * @param qtyDecimals Step precision for the symbol.
	 * @return The parsed snapshot, or an error message on malformed input.
	 * @see parse_binance_depth
	 */
	[[nodiscard]] std::expected<depth_snapshot, depth_parse_error>
	parse_snapshot(std::string_view json, int priceDecimals, int qtyDecimals);

	/**
	 * @brief Parse one @c depthUpdate frame, reusing this parser's buffers.
	 * @param json The raw JSON of a single @c depthUpdate frame.
	 * @param priceDecimals Tick precision for the symbol.
	 * @param qtyDecimals Step precision for the symbol.
	 * @return The parsed diff event, or an error message on malformed input.
	 * @see parse_binance_depth_update
	 */
	[[nodiscard]] std::expected<depth_update, depth_parse_error>
	parse_update(std::string_view json, int priceDecimals, int qtyDecimals);

	/**
	 * @brief Parse one @c depthUpdate frame and stream its levels straight into
	 *        @p book, reusing this parser's buffers.
	 *
	 * The hot-path form of @c apply_binance_depth_update: it reuses the parser
	 * and input buffer across frames @b and skips the per-frame level vectors.
	 * @param book The book to mutate.
	 * @param json The raw JSON of a single @c depthUpdate frame.
	 * @param priceDecimals Tick precision for the symbol.
	 * @param qtyDecimals Step precision for the symbol.
	 * @return The update's ids/time, or an error message on malformed input.
	 * @warning Not atomic (see @c apply_binance_depth_update).
	 */
	[[nodiscard]] MARKET_DATA_EXPORT
		std::expected<depth_update_meta, depth_parse_error>
		apply_update(l2_book &book, std::string_view json, int priceDecimals,
					 int qtyDecimals);

private:
	struct impl;
	core::util::indirect<impl> impl_;
};

} // namespace exchange::market_data::binance
