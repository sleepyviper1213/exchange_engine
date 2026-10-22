// Reads of the account outside the session: the sweeps either side of a
// run, the balance check before it, and fills the stream never delivered.

#include "serve_internal.hpp"

#include "app/commands/venue_orders.hpp"
#include "core/concurrency/affinity/format.hpp" // IWYU pragma: keep - fmt::formatter<topology>
#include "core/metrics/format.hpp" // IWYU pragma: keep - fmt::formatter<registry>
#include "core/scaled/decimal.hpp"
#include "core/scaled/fixed_point.hpp"
#include "format.hpp" // IWYU pragma: keep - fmt::formatter<startup>, <shutdown>
#include "market_data/format.hpp" // IWYU pragma: keep - fmt::formatter<depth_speed>
#include "symbol/symbol_spec.hpp"
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
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

using namespace exchange::engine;

namespace exchange::app::serve_detail {

void sweep_resting_orders(const serve_settings &settings,
						  std::string_view phase) {
	const venue_access access{.symbol       = settings.symbol,
							  .credential   = settings.credential,
							  .env          = settings.env,
							  .insecure_tls = false};
	venue::weight_budget budget;
	const auto open = read_open_orders(access, budget);
	if (!open) {
		spdlog::warn("{} sweep: could not read the venue's open orders - run "
					 "`account --cancel-all` to make sure nothing is resting",
					 phase);
		return;
	}

	const auto found = session::reconcile({}, *open);
	const auto ours  = std::ranges::count_if(found, [](const auto &entry) {
		return entry.finding != session::reconciliation::foreign;
	});
	if (ours == 0) {
		spdlog::info("{} sweep: nothing of ours working at the venue", phase);
		return;
	}
	spdlog::warn("{} sweep: {} order(s) of ours working that this run is not "
				 "managing; cancelling",
				 phase,
				 ours);
	report_open(found);
	if (!cancel_ours(found, access, budget))
		spdlog::error("{} sweep: not everything could be withdrawn - run "
					  "`account --cancel-all`",
					  phase);
}

[[nodiscard]] bool can_afford_either_side(
	const serve_settings &settings,
	const std::optional<venue::binance::symbol_filters> &grid,
	const symbol_spec &spec) {
	if (!grid || grid->base_asset.empty() || grid->quote_asset.empty())
		return true;
	const venue_access access{.symbol     = settings.symbol,
							  .credential = settings.credential,
							  .env        = settings.env};
	venue::weight_budget budget;
	const auto balances = read_balances(access, budget);
	const auto last     = read_last_price(access);
	if (!balances || !last) {
		spdlog::warn("could not check the account's balances; going ahead");
		return true;
	}

	const auto free_of = [&](const std::string &asset, int scale) {
		for (const auto &held : *balances)
			if (held.asset == asset)
				return core::scaled::parse_fixed_point(held.free, scale)
					.value_or(0);
		return std::int64_t{0};
	};
	const std::int64_t qty = scaled_of(spec.quantity_to_scaled(
		static_cast<quantity_t::rep>(settings.lots) * units::lot));
	const std::int64_t price =
		core::scaled::parse_fixed_point(*last, spec.price_scale()).value_or(0);
	const std::int64_t base_free = free_of(grid->base_asset, spec.qty_scale());
	const std::int64_t quote_free =
		free_of(grid->quote_asset, spec.price_scale() + spec.qty_scale());

	const bool can_sell = base_free >= qty;
	const bool can_buy  = price > 0 && quote_free >= price * qty;
	spdlog::info(
		"balances: {} {}, {} {}; one order is {} {} at about {}",
		core::scaled::to_decimal(base_free, spec.qty_scale()),
		grid->base_asset,
		core::scaled::to_decimal(quote_free,
								 spec.price_scale() + spec.qty_scale()),
		grid->quote_asset,
		core::scaled::to_decimal(qty, spec.qty_scale()),
		grid->base_asset,
		*last);
	if (!can_sell)
		spdlog::warn(
			"the account cannot cover one sell - asks will be refused "
			"until it holds more {}, and the quoter backs that side off",
			grid->base_asset);
	if (!can_buy)
		spdlog::warn(
			"the account cannot cover one buy - bids will be refused "
			"until it holds more {}, and the quoter backs that side off",
			grid->quote_asset);
	if (!can_sell && !can_buy) {
		spdlog::error("the account can pay for neither side of a {}-lot quote; "
					  "nothing this run placed could be accepted",
					  settings.lots);
		return false;
	}
	return true;
}

[[nodiscard]] std::int64_t
account_for_unreported_fills(const serve_settings &settings,
							 const serving_session &run) {
	if (run.newest_trade_id() < 0) return 0;
	const venue_access access{.symbol     = settings.symbol,
							  .credential = settings.credential,
							  .env        = settings.env};
	venue::weight_budget budget;
	const auto trades =
		read_trades_from(access, run.newest_trade_id() + 1, budget);
	if (!trades) {
		spdlog::warn("could not read the trade history - fills during the "
					 "exit, if any, are missing from the position below");
		return 0;
	}

	std::int64_t net = 0;
	for (const auto &trade : *trades) {
		const auto client =
			read_client_order_id(access, trade.order_id, budget);
		if (!client || !session::is_ours(*client)) continue;
		const auto lots = run.spec().quantity_from_text(trade.qty);
		if (!lots) continue;
		const std::int64_t signed_lots =
			(trade.is_buyer ? 1 : -1) * lots->numerical_value_in(units::lot);
		net += signed_lots;
		spdlog::warn("fill during the exit, after the account stream stopped: "
					 "{} {} {} @ {} ({})",
					 trade.is_buyer ? "bought" : "sold",
					 trade.qty,
					 settings.symbol,
					 trade.price,
					 *client);
	}
	if (net != 0)
		spdlog::warn("the position at exit, with those fills: {} lots",
					 run.positions()
							 .net_lots(run.symbol())
							 .numerical_value_in(units::lot) +
						 net);
	return net;
}

} // namespace exchange::app::serve_detail
