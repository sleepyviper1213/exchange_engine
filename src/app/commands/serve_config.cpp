// Building the run from the command line and the venue's reference data:
// the listing, the feed options, the loss floor and the session policy.

#include "serve_internal.hpp"

#include "core/concurrency/affinity/format.hpp" // IWYU pragma: keep - fmt::formatter<topology>
#include "core/metrics/format.hpp" // IWYU pragma: keep - fmt::formatter<registry>
#include "core/scaled/decimal.hpp"
#include "core/scaled/fixed_point.hpp"
#include "format.hpp" // IWYU pragma: keep - fmt::formatter<startup>, <shutdown>
#include "increment.hpp"
#include "market_data/binance/depth_speed.hpp"
#include "market_data/format.hpp" // IWYU pragma: keep - fmt::formatter<depth_speed>
#include "session/reaction_metrics.hpp"
#include "symbol/symbol_spec.hpp"
#include "transport/rest/pipeline.hpp"
#include "venue/binance/api_error.hpp"
#include "venue/binance/exchange_info.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <fmt/ranges.h>
#include <fmt/std.h> // IWYU pragma: keep - fmt::formatter<std::filesystem::path>
#include <spdlog/spdlog.h>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace exchange::engine;

namespace exchange::app::serve_detail {

[[nodiscard]] std::optional<venue::binance::symbol_filters>
fetch_venue_grid(const serve_settings &settings) {
	namespace binance = market_data::binance;

	auto [host, target] =
		venue::binance::exchange_info_endpoint(settings.symbol, settings.env);
	spdlog::debug("GET {}{}", host, target);
	const auto fetched =
		transport::rest::get(std::move(host), std::move(target));
	if (!fetched) {
		spdlog::warn(
			"could not read {}'s grid from the venue: {}",
			settings.symbol,
			venue::binance::describe_api_error(fetched.error().body,
											   fetched.error().message()));
		return std::nullopt;
	}

	auto filters =
		venue::binance::parse_exchange_info(fetched->body, settings.symbol);
	if (!filters) {
		spdlog::warn("could not read {}'s grid from the venue: {}",
					 settings.symbol,
					 filters.error());
		return std::nullopt;
	}

	// Logged whether or not it differs, because "the venue says the same thing"
	// is the reassurance an operator wants and costs one line. A difference is
	// escalated, since it means every size in a run configured the old way was
	// quantised on the wrong grid.
	spdlog::info(
		"venue grid for {}: tick {} ({} dp), step {} ({} dp), status {}",
		filters->symbol,
		filters->tick_size,
		filters->price_decimals,
		filters->step_size,
		filters->qty_decimals,
		filters->status);
	if (filters->qty_decimals != settings.qty_decimals)
		spdlog::warn("--qty-decimals {} would have truncated sizes the venue "
					 "publishes to {} decimals; using the venue's",
					 settings.qty_decimals,
					 filters->qty_decimals);
	if (!filters->is_trading())
		spdlog::warn("{} is {} rather than TRADING - the feed may be idle",
					 filters->symbol,
					 filters->status);
	return *filters;
}

[[nodiscard]] std::optional<symbol_spec>
make_listing(const serve_settings &settings,
			 const std::optional<venue::binance::symbol_filters> &grid) {
	// The venue's grid wins where we have it, because it is the grid the
	// numbers on the wire are quantised to and the flags are a guess at it.
	// Precision is the half that matters: surplus decimals are *truncated* on
	// the way in, so a step configured coarser than the venue's silently rounds
	// small levels to nothing. @see venue::binance::symbol_filters
	//
	// Named `grid` and not `venue`: the latter now names a namespace, and a
	// parameter shadowing it would hide every venue:: lookup in this function.
	const std::string tick = grid ? grid->tick_size : settings.tick;
	const std::string lot  = grid ? grid->step_size : settings.lot;
	const int price_decimals =
		grid ? grid->price_decimals : settings.price_decimals;
	const int qty_decimals = grid ? grid->qty_decimals : settings.qty_decimals;

	const auto tick_scaled = increment(tick, price_decimals, "tick");
	const auto lot_scaled  = increment(lot, qty_decimals, "lot");
	if (!tick_scaled || !lot_scaled) return std::nullopt;

	// Uncollared, so the anchor is inert - one tick is the smallest on-grid
	// value and is honest about being a placeholder, where a guess at the
	// instrument's price would look like a fact. @see serve_settings::reference
	std::int64_t reference = *tick_scaled;
	if (!settings.reference.empty()) {
		const auto given =
			increment(settings.reference, price_decimals, "reference");
		if (!given) return std::nullopt;
		if (*given % *tick_scaled != 0) {
			spdlog::error("--reference '{}' is not on the tick grid",
						  settings.reference);
			return std::nullopt;
		}
		reference = *given;
	}

	return symbol_spec{0,
					   settings.symbol,
					   price_decimals,
					   qty_decimals,
					   *tick_scaled * units::scaled_price,
					   *lot_scaled * units::scaled_size,
					   at_scaled(reference)};
}

[[nodiscard]] live_feed_options
feed_options_from(const serve_settings &settings,
				  market_data::binance::depth_speed cadence) {
	live_feed_options options;
	options.duration       = std::chrono::seconds(settings.seconds);
	options.limit          = settings.limit;
	options.price_decimals = settings.price_decimals;
	options.qty_decimals   = settings.qty_decimals;
	options.speed          = cadence;
	// The one value the depth stream, the snapshot fetch and the order path all
	// read. They have to agree: testnet keeps its own book, so a run reading
	// production depth while placing orders there is a strategy reacting to a
	// market it is not trading in. @see venue::environment
	options.env    = settings.env;
	options.verify = settings.insecure_tls ? transport::tls_verify::none
										   : transport::tls_verify::peer;
	options.reconnect_delay = std::chrono::milliseconds(settings.reconnect_ms);
	options.max_reconnects  = settings.max_reconnects;
	return options;
}

[[nodiscard]] std::int64_t
notional_floor(const std::optional<venue::binance::symbol_filters> &grid,
			   const symbol_spec &spec) {
	if (!grid || grid->min_notional.empty()) return 0;
	const int scale = spec.price_scale() + spec.qty_scale();
	const auto floor =
		core::scaled::parse_fixed_point(grid->min_notional, scale);
	if (!floor) {
		// Unreadable rather than absent, which is a different thing and worth
		// a line: the filter was there and we could not use it, so the check
		// this run makes is weaker than the one it was meant to make.
		spdlog::warn("could not read {}'s minimum notional '{}' - orders will "
					 "not be checked against it before sending",
					 grid->symbol,
					 grid->min_notional);
		return 0;
	}
	spdlog::info("venue minimum order value for {}: {} - anything smaller is "
				 "refused here rather than by the venue",
				 grid->symbol,
				 grid->min_notional);
	return *floor;
}

[[nodiscard]] std::optional<notional_t>
loss_floor(const serve_settings &settings,
		   const std::optional<venue::binance::symbol_filters> &grid,
		   const symbol_spec &spec) {
	if (settings.max_loss.empty()) return risk::risk_limits::NO_LOSS_LIMIT;
	const auto parsed =
		engine::parse_exact_decimal(settings.max_loss, USDT_SCALE);
	if (!parsed) {
		spdlog::error("--max-loss '{}': {}",
					  settings.max_loss,
					  engine::describe(parsed.error()));
		return std::nullopt;
	}
	if (*parsed == 0) return risk::risk_limits::NO_LOSS_LIMIT;

	// usdt_t is tether and nothing else, so a listing quoted in something else
	// has no loss this flag can state.
	if (grid && !grid->quote_asset.empty() && grid->quote_asset != "USDT") {
		spdlog::error("--max-loss is in USDT, and {} is quoted in {}",
					  grid->symbol,
					  grid->quote_asset);
		return std::nullopt;
	}
	if (!grid || grid->quote_asset.empty())
		spdlog::warn("the venue did not say what {} is quoted in; reading "
					 "--max-loss as USDT",
					 spec.symbol());

	const usdt_t stated = *parsed * units::usdt_e8;
	const auto floor    = spec.notional_within(stated);
	if (!floor) {
		spdlog::error("--max-loss needs a listing whose tick-lot is a whole "
					  "number of 1e-8 USDT, and {} at {} + {} decimals is not",
					  spec.symbol(),
					  spec.price_scale(),
					  spec.qty_scale());
		return std::nullopt;
	}
	// Rounded toward zero, so a loss under one tick-lot becomes zero - which
	// risk_limits reads as "no floor". Refused rather than disabled.
	if (*floor == risk::risk_limits::NO_LOSS_LIMIT) {
		spdlog::error("--max-loss {} USDT is less than one tick-lot of {}",
					  settings.max_loss,
					  spec.symbol());
		return std::nullopt;
	}
	spdlog::info("loss floor: {} USDT, checked as {} tick-lots",
				 core::scaled::to_decimal(usdt_e8_of(stated), USDT_SCALE),
				 floor->numerical_value_in(units::tick * units::lot));
	return *floor;
}

[[nodiscard]] live_session_options
policy_from(const serve_settings &settings, notional_t max_loss,
			execution::partition_metrics *metrics,
			session::reaction_metrics *reaction) {
	live_session_options options;

	options.quoting.improve_ticks =
		static_cast<price_t::rep>(settings.improve_ticks) * units::tick;
	options.quoting.lots =
		static_cast<quantity_t::rep>(settings.lots) * units::lot;
	options.quoting.requote_interval_ns =
		to_ns(std::chrono::milliseconds{settings.requote_ms});
	options.quoting.take_liquidity = settings.take;

	// Both knobs read the flag's *negation*: the model's own defaults are the
	// conservative ones, and a switch a user has to set in order to be
	// flattered is the right way round for a number anybody will act on.
	options.simulate_fills              = settings.simulate_fills;
	options.fills.require_trade_through = !settings.fill_on_lock;
	options.fills.model_queue_position  = !settings.front_of_queue;

	// Left all-zero unless asked for, which builds no wire at all rather than a
	// wire with a zero delay. @see session::latency_pipe
	options.latency.order_entry_ns = settings.latency_ns;
	options.latency.jitter_ns      = settings.jitter_ns;
	if (settings.seed != 0) options.latency.seed = settings.seed;

	if (settings.max_order_qty > 0)
		options.limits.max_order_qty =
			static_cast<quantity_t::rep>(settings.max_order_qty) * units::lot;
	if (settings.max_position > 0)
		options.limits.max_position_lots = settings.max_position * units::lot;
	options.limits.price_band_bps = settings.price_band_bps;
	options.limits.max_loss       = max_loss;
	options.breaches_to_trip      = settings.breaches_to_trip;

	options.surveillance.max_messages_per_execution =
		settings.max_messages_per_execution;
	options.surveillance.max_executions_per_window =
		settings.max_executions_per_window;
	options.surveillance.max_adverse_run = settings.max_adverse_run;
	if (settings.burst_window_ms != 0)
		options.surveillance.burst_window_log2_ns = window_log2_for(
			std::chrono::milliseconds{settings.burst_window_ms});
	if (settings.otr_window_ms != 0)
		options.surveillance.ratio_window_log2_ns =
			window_log2_for(std::chrono::milliseconds{settings.otr_window_ms});
	if (settings.min_otr_messages != 0)
		options.surveillance.min_messages_to_judge = settings.min_otr_messages;
	options.surveillance.outcome_timeout_ns =
		to_ns(std::chrono::milliseconds{settings.outcome_timeout_ms});

	options.feed_timeout_ns =
		to_ns(std::chrono::milliseconds{settings.feed_timeout_ms});
	options.max_feed_lag_ns =
		to_ns(std::chrono::milliseconds{settings.max_feed_lag_ms});
	// Read only by a session that is later given a gateway, which is what keeps
	// a run that did not ask for order entry the run it has always been.
	options.venue_symbol = settings.symbol;

	options.metrics  = metrics;
	options.reaction = reaction;
	return options;
}

} // namespace exchange::app::serve_detail
