# Strategies, the host that runs them, and the offline backtest harness under
# backtest/. Genuinely header-only, unlike risk_management next door: every
# strategy is composed by value into a `strategy_engine` tuple so each hook is a
# direct call, and the harness is templated on the trader it drives. There is
# nothing to compile that is not already in a header.
#
# An INTERFACE target rather than no target at all, so the dependencies are
# declared where the build can check them instead of being inherited by accident
# from whoever happens to link this in.
add_library(strategy INTERFACE)
add_library(exchange_engine::strategy ALIAS strategy)

target_include_directories(strategy
	INTERFACE
	$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
	$<INSTALL_INTERFACE:include>
)

# market_data is here for the two headers that read a venue's reconstructed
# depth: backtest/ (depth_feed_bridge and the fill model) and quoter.hpp, whose
# market hook takes an l2_book. This is the module that joins market-data to the
# matching engine now that the bridge lives beside its caller. A translation
# unit that only defines a strategy includes none of it — which is why both
# strategy/backtest.hpp and strategy/quoter.hpp are kept out of strategy.hpp.
target_link_libraries(strategy
	INTERFACE
	exchange_engine::risk_management
	exchange_engine::market_data
	exchange_engine::core
)
