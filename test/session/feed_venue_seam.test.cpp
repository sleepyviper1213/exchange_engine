#include "market_data/depth_venue.hpp"
#include "market_data/reconstructor.hpp"
#include "session/live_feed.hpp"
#include "session/user_data_feed.hpp"
#include "venue/account_stream.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

// A second venue is a second model, not an edit to either live pipeline.
//
// Both feeds - the depth stream and the account stream - are templates over a
// venue concept, with Binance as the default. These stubs are venues that are
// not Binance. Running a pipeline needs a socket, so what is pinned here is the
// seam's shape - both concepts admit a non-Binance model, both pipelines
// instantiate over one, and the options carry the venue's own cadence type -
// rather than the behaviour, which the Binance suites and a testnet run cover.

using exchange::market_data::book_snapshot;
using exchange::market_data::depth_event;
using exchange::market_data::depth_reconstructor;
using exchange::market_data::feed_status;
using exchange::session::basic_live_feed_options;
using exchange::session::detail::live_pipeline;
using exchange::session::user_data_pipeline;
using exchange::venue::credentials;
using exchange::venue::environment;
using exchange::venue::execution_report;
using exchange::venue::http_endpoint;
using exchange::venue::stream_endpoint;

namespace {

struct seam_stub_depth_decoder {
	seam_stub_depth_decoder(int, int) {}
	std::expected<depth_event, feed_status>
	decode(std::string_view, std::uint64_t = 0,
		   exchange::core::chrono::ingress_time = {}) {
		return std::unexpected(feed_status{});
	}
};

struct seam_stub_depth_venue {
	using cadence = int;
	using decoder = seam_stub_depth_decoder;
	static constexpr cadence DEFAULT_CADENCE = 250;

	static stream_endpoint stream(std::string_view, cadence, environment) {
		return {.host = "stub.example", .port = "443", .target = "/depth"};
	}
	static http_endpoint snapshot_endpoint(std::string_view, int, environment) {
		return {.host = "stub.example", .target = "/snapshot"};
	}
	static std::expected<book_snapshot, std::string>
	parse_snapshot(std::string_view, int, int) {
		return std::unexpected(std::string{"stub"});
	}
	static std::string describe_error(std::string_view, std::string_view why) {
		return std::string{why};
	}
};

enum class seam_stub_error : std::uint8_t { other, broken };

struct seam_stub_account {
	using error_type = seam_stub_error;
	static stream_endpoint endpoint(environment) {
		return {.host = "stub.example", .port = "443", .target = "/account"};
	}
	static std::expected<std::string, error_type>
	subscribe_request(const credentials &, std::int64_t, std::string_view) {
		return std::string{"{}"};
	}
	static bool is_event(std::string_view) { return true; }
	static std::expected<std::int64_t, error_type>
	parse_subscribe_reply(std::string_view) {
		return 1;
	}
	static std::expected<execution_report, error_type>
	parse_report(std::string_view, int, int) {
		return std::unexpected(seam_stub_error::other);
	}
	static bool is_other_event(error_type e) {
		return e == seam_stub_error::other;
	}
	static std::string_view describe(error_type) { return "stub"; }
};

struct seam_stub_account_handler {
	void on_report(const execution_report &) {}
	void on_gap(std::string_view) {}
};

static_assert(exchange::market_data::depth_venue<seam_stub_depth_venue>);
static_assert(exchange::venue::account_stream<seam_stub_account>);

static_assert(sizeof(live_pipeline<depth_reconstructor, seam_stub_depth_venue>) > 0);
static_assert(sizeof(user_data_pipeline<seam_stub_account_handler,
										 seam_stub_account>) > 0);

} // namespace

TEST(FeedVenueSeam, OptionsTakeTheirCadenceFromTheVenue) {
	EXPECT_EQ(basic_live_feed_options<seam_stub_depth_venue>{}.speed, 250);
}
