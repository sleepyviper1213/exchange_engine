#pragma once
// What a venue's depth feed has to supply for a live pipeline to keep a
// replica current: where the diff stream and the snapshot are, how to read
// each, and how to say what went wrong.
//
// The live pipeline in `session/live_feed.hpp` is a template over this, so the
// procedure - subscribe first, fetch a snapshot while frames keep arriving,
// drain it into the frame loop, reconnect - is written once and a second venue
// is a second model rather than an edit to the pipeline. It sits beside
// `feed.hpp` for the same reason: `feed.hpp` is the offline seam a venue plugs
// into, and this is the live one.

#include "book_snapshot.hpp"
#include "core/chrono/ingress.hpp"
#include "depth_event.hpp"
#include "feed.hpp"
#include "venue/environment.hpp"

#include <concepts>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace exchange::market_data {

/**
 * @brief A venue's live depth feed.
 *
 * @c D::stream returns something with @c host, @c port and @c target;
 * @c D::snapshot_endpoint something with @c host and @c target.
 * @c D::decoder is built from the listing's two precisions and turns one frame
 * into a neutral @c depth_event - the same shape the offline @c depth_feed
 * produces, which is what lets one @c depth_reconstructor serve both.
 */
template <class D>
concept depth_venue =
	std::constructible_from<typename D::decoder, int, int> &&
	requires(std::string_view text, typename D::cadence cadence,
			 venue::environment env, int scale, typename D::decoder &decoder,
			 std::uint64_t position, core::chrono::ingress_time at) {
		{ D::DEFAULT_CADENCE } -> std::convertible_to<typename D::cadence>;
		{ D::stream(text, cadence, env).target };
		{ D::stream(text, cadence, env).port };
		{ D::snapshot_endpoint(text, scale, env).target };
		{ D::parse_snapshot(text, scale, scale) }
			-> std::same_as<std::expected<book_snapshot, std::string>>;
		{ D::describe_error(text, text) } -> std::convertible_to<std::string>;
		{ decoder.decode(text, position, at) }
			-> std::same_as<std::expected<depth_event, feed_status>>;
	};

} // namespace exchange::market_data
