#include "io/binance_depth.hpp"

#include <utility>
#include <simdjson.h>
#include <fmt/format.h>

namespace binance {
    namespace {
        using simdjson::fallback::numberparsing::is_digit;

        // One level as raw decimal strings, before scaling. The string_views point into
        // the parser's padded buffer, which outlives this whole parse.
        struct RawLevel {
            std::string_view price;
            std::string_view qty;
        };

        /// Read a bids/asks array of ["price","qty"] string pairs into scaled levels.
        ///
        /// On-Demand iteration is carried to completion *before* any fallible numeric
        /// conversion runs: abandoning the iterator mid-stream (e.g. on a malformed
        /// number) would leave simdjson's depth bookkeeping inconsistent and trip its
        /// debug assertions. So we collect the raw string fields in one pass, then
        /// scale them in a second pass once no iterator is in flight.
        std::expected<std::vector<PriceLevel>, std::string>
        parse_levels(simdjson::ondemand::value array_value, int price_decimals,
                     int qty_decimals) {
            using namespace simdjson;

            ondemand::array array;
            if (const auto err = array_value.get_array().get(array)) {
                return std::unexpected(error_message(err));
            }

            // Pass 1: drain the On-Demand iterator, capturing raw decimal strings.
            std::vector<RawLevel> raw;
            for (auto element: array) {
                ondemand::array pair;
                if (const auto err = element.get_array().get(pair)) {
                    return std::unexpected(error_message(err));
                }

                std::string_view fields[2];
                int count = 0;
                for (auto field: pair) {
                    std::string_view sv;
                    if (const auto err = field.get_string().get(sv)) {
                        return std::unexpected(error_message(err));
                    }
                    if (count < 2) fields[count] = sv;
                    ++count;
                }
                if (count != 2)
                    return std::unexpected(
                        "level is not a [price, qty] pair");

                raw.push_back(RawLevel{fields[0], fields[1]});
            }

            // Pass 2: scale to integers now that iteration is fully done.
            std::vector<PriceLevel> levels;
            levels.reserve(raw.size());
            for (const auto &[price_text, qty_text]: raw) {
                const auto price = parse_scaled(price_text, price_decimals);
                if (!price) return std::unexpected(price.error());
                const auto qty = parse_scaled(qty_text, qty_decimals);
                if (!qty) return std::unexpected(qty.error());

                levels.emplace_back(
                    static_cast<Price>(*price),
                    static_cast<Volume>(*qty)
                );
            }
            return levels;
        }

        /**
         * @brief Read and scale the two level arrays of a depth document.
         *
         * The caller must have consumed any scalar fields first, since simdjson
         * On-Demand walks the document in order and does not rewind.
         * @param doc An already-iterated depth document.
         * @param bid_key Field name of the bids array (@c "bids" or @c "b").
         * @param ask_key Field name of the asks array (@c "asks" or @c "a").
         * @param price_decimals Tick precision to scale prices by.
         * @param qty_decimals Step precision to scale quantities by.
         * @return A {bids, asks} pair of scaled levels, or an error message
         *         prefixed with the offending field name.
         */
        std::expected<std::pair<std::vector<PriceLevel>, std::vector<PriceLevel>>,
                      std::string>
        parse_sides(simdjson::ondemand::document &doc, std::string_view bid_key,
                    std::string_view ask_key, int price_decimals,
                    int qty_decimals) {
            using namespace simdjson;

            ondemand::value bids_value;
            if (const auto err = doc[bid_key].get(bids_value)) {
                return std::unexpected(std::string(bid_key) + ": " +
                                       error_message(err));
            }
            auto bids = parse_levels(bids_value, price_decimals, qty_decimals);
            if (!bids) return std::unexpected(bids.error());

            ondemand::value asks_value;
            if (const auto err = doc[ask_key].get(asks_value)) {
                return std::unexpected(std::string(ask_key) + ": " +
                                       error_message(err));
            }
            auto asks = parse_levels(asks_value, price_decimals, qty_decimals);
            if (!asks) return std::unexpected(asks.error());

            return std::pair{std::move(*bids), std::move(*asks)};
        }
    } // namespace

    std::expected<std::int64_t, std::string>
    parse_scaled(std::string_view text, int decimals) {
        if (decimals < 0) return std::unexpected("negative decimals");
        if (text.empty()) return std::unexpected("empty number");

        std::size_t i = 0;
        bool negative = false;
        if (text[i] == '+' || text[i] == '-') {
            negative = text[i] == '-';
            ++i;
        }

        std::int64_t value = 0;
        bool any_digit = false;
        for (; i < text.size() && text[i] != '.'; ++i) {
            if (!is_digit(text[i]))
                return std::unexpected(
                    "invalid digit in number");
            value = value * 10 + (text[i] - '0');
            any_digit = true;
        }

        int consumed = 0;
        if (i < text.size() && text[i] == '.') {
            ++i;
            for (; i < text.size() && consumed < decimals; ++i, ++consumed) {
                if (!is_digit(text[i]))
                    return std::unexpected(
                        "invalid digit in number");
                value = value * 10 + (text[i] - '0');
                any_digit = true;
            }
            // Validate (and truncate) any remaining fractional digits.
            for (; i < text.size(); ++i) {
                if (!is_digit(text[i]))
                    return std::unexpected(
                        "invalid trailing char");
            }
        }
        if (!any_digit) return std::unexpected("no digits in number");

        for (; consumed < decimals; ++consumed) value *= 10; // zero-pad
        return negative ? -value : value;
    }

    std::expected<DepthSnapshot, std::string>
    parse_binance_depth(std::string_view json, int price_decimals,
                        int qty_decimals) {
        using namespace simdjson;

        // simdjson On-Demand needs SIMDJSON_PADDING bytes past the end; copy into a
        // padded buffer. Fields are read in document order (lastUpdateId, bids, asks)
        // so On-Demand never rewinds.
        ondemand::parser parser;
        padded_string padded(json);
        ondemand::document doc;
        if (const auto err = parser.iterate(padded).get(doc)) {
            return std::unexpected(error_message(err));
        }

        DepthSnapshot snapshot;
        if (std::uint64_t id; !doc["lastUpdateId"].get(id)) {
            snapshot.lastUpdateId = id; // absent/typed-wrong -> leave 0
        }

        auto sides = parse_sides(doc, "bids", "asks", price_decimals,
                                 qty_decimals);
        if (!sides) return std::unexpected(sides.error());

        snapshot.bids = std::move(sides->first);
        snapshot.asks = std::move(sides->second);
        return snapshot;
    }

    std::expected<DepthUpdate, std::string>
    parse_binance_depth_update(std::string_view json, int price_decimals,
                               int qty_decimals) {
        using namespace simdjson;

        // Same padded-buffer contract as parse_binance_depth. depthUpdate frames
        // arrive as {e,E,s,U,u,b,a}; operator[] tolerates the leading e/s we skip,
        // and we read the rest (E, U, u, b, a) in document order.
        ondemand::parser parser;
        padded_string padded(json);
        ondemand::document doc;
        if (const auto err = parser.iterate(padded).get(doc)) {
            return std::unexpected(error_message(err));
        }

        DepthUpdate update;
        if (std::uint64_t event_time; !doc["E"].get(event_time)) {
            update.eventTime = event_time; // absent/typed-wrong -> leave 0
        }
        if (std::uint64_t first_id; !doc["U"].get(first_id)) {
            update.firstUpdateId = first_id;
        }
        if (std::uint64_t final_id; !doc["u"].get(final_id)) {
            update.finalUpdateId = final_id;
        }

        auto sides = parse_sides(doc, "b", "a", price_decimals, qty_decimals);
        if (!sides) return std::unexpected(sides.error());

        update.bids = std::move(sides->first);
        update.asks = std::move(sides->second);
        return update;
    }

    std::expected<std::vector<DepthUpdate>, std::string>
    parse_binance_depth_updates(std::string_view jsonl, int price_decimals,
                                int qty_decimals) {
        std::vector<DepthUpdate> updates;
        std::size_t line_no = 0;
        std::size_t pos = 0;

        while (pos <= jsonl.size()) {
            const std::size_t nl = jsonl.find('\n', pos);
            const std::size_t end = nl == std::string_view::npos
                                        ? jsonl.size()
                                        : nl;
            std::string_view line = jsonl.substr(pos, end - pos);
            ++line_no;

            // Trim a trailing '\r' so CRLF captures parse, and skip blank lines.
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            const auto first = line.find_first_not_of(" \t");
            if (first != std::string_view::npos) {
                auto parsed = parse_binance_depth_update(line, price_decimals,
                                                         qty_decimals);
                if (!parsed) {
                    return std::unexpected(fmt::format(
                        "line {}: ", line_no,
                        parsed.error()));
                }
                updates.push_back(std::move(*parsed));
            }

            if (nl == std::string_view::npos) break;
            pos = nl + 1;
        }
        return updates;
    }
} // namespace binance
