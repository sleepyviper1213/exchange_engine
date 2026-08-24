# The test tree mirrors src/, and every file holds exactly one gtest suite:
#
#     <module>/<group>/<suite>.test.cpp
#     <module>/<group>/<component>/<aspect>.test.cpp
#
# So a suite never spans two files and a file never holds two suites, and the
# path says which module and component a failure came from before you open
# anything. Helpers shared by sibling suites live in a `<component>.fixture.hpp`
# next to them rather than being copied.
#
# A component earns the second form once it has enough suites that the shared
# prefix is noise: `engine_partition/records` reads better than
# `engine_partition_records`, and the folder is where the fixture then lives. The
# suite name keeps the full prefix either way — the file is
# `engine_partition/drain.test.cpp` and the suite inside it is
# `EnginePartitionDrain`, because a gtest filter has no folders to lean on.
#
# One binary rather than one per subsystem: the suite runs in ~8 seconds and a
# single target keeps the link configuration in one place. Sources are grouped
# below in src/ order.

add_executable(order_test
    app/ledger_view.test.cpp
    app/live_session.test.cpp
    app/live_session_fills.test.cpp
    app/live_session_realtime.test.cpp

    core/concurrency/affinity/core_allocator.test.cpp
    core/concurrency/affinity/topology.test.cpp
    core/concurrency/affinity/topology_format.test.cpp

    core/concurrency/lockfree/fast_queue.test.cpp
    core/concurrency/lockfree/hazard_stack.test.cpp
    core/concurrency/lockfree/spsc_queue_capacity_one.test.cpp
    core/concurrency/lockfree/spsc_queue_clear.test.cpp
    core/concurrency/lockfree/spsc_queue_concurrency.test.cpp
    core/concurrency/lockfree/spsc_queue_consume_all.test.cpp
    core/concurrency/lockfree/spsc_queue_consume_up_to.test.cpp
    core/concurrency/lockfree/spsc_queue_non_pod.test.cpp
    core/concurrency/lockfree/spsc_queue_observers.test.cpp
    core/concurrency/lockfree/spsc_queue_try_dequeue_optional.test.cpp
    core/concurrency/lockfree/spsc_queue_try_dequeue_out_param.test.cpp
    core/concurrency/lockfree/spsc_queue_try_dequeue_range.test.cpp
    core/concurrency/lockfree/spsc_queue_try_emplace.test.cpp
    core/concurrency/lockfree/spsc_queue_try_emplace_range.test.cpp
    core/concurrency/lockfree/spsc_queue_try_pop_out_param.test.cpp
    core/concurrency/lockfree/wait_free_hash_map.test.cpp

    core/concurrency/synchronisation/hazard_pointer.test.cpp

    core/logging/structured.test.cpp

    core/memory/allocator.test.cpp
    core/memory/node_pool.test.cpp
    core/memory/object_pool.test.cpp
    core/memory/slab.test.cpp

    core/metrics/counter.test.cpp
    core/metrics/histogram.test.cpp
    core/metrics/histogram_format.test.cpp
    core/metrics/registry.test.cpp
    core/metrics/registry_format.test.cpp
    core/metrics/sla_monitor.test.cpp

    core/optimisation/branchless_lower_bound.test.cpp

    core/persistence/event_store.test.cpp
    core/persistence/manifest.test.cpp
    core/persistence/record_log.test.cpp
    core/persistence/replay.test.cpp

    core/util/enum_conversion.test.cpp
    core/util/enum_parse.test.cpp
    core/util/flag.test.cpp
    core/util/function_ref.test.cpp
    core/util/format_as.test.cpp
    core/util/nested_formatter_contract.test.cpp

    market-data/binance/apply_depth_update.test.cpp
    market-data/binance/depth_snapshot_endpoint.test.cpp
    market-data/binance/depth_frame_decoder.test.cpp
    market-data/binance/depth_speed.test.cpp
    market-data/binance/diff_depth_stream.test.cpp
    market-data/binance/exchange_info_endpoint.test.cpp
    market-data/binance/jsonl_depth_feed.test.cpp
    market-data/binance/live_api.test.cpp
    market-data/binance/normalise.test.cpp
    market-data/binance/parse_api_error.test.cpp
    market-data/binance/parse_binance_depth.test.cpp
    market-data/binance/parse_depth_update.test.cpp
    market-data/binance/parse_depth_updates.test.cpp
    market-data/binance/parse_exchange_info.test.cpp
    market-data/binance/parse_scaled.test.cpp
    market-data/binance/significant_decimals.test.cpp

    market-data/book/l2_book.test.cpp
    market-data/book/load_side.test.cpp

    market-data/feed/apply_event.test.cpp
    market-data/feed/depth_reconstructor.test.cpp
    market-data/feed/depth_sequencer.test.cpp
    market-data/feed/drive.test.cpp
    market-data/feed/replay_feed.test.cpp
    market-data/feed/reset_book.test.cpp

    market-data/format/depth_parse_error_format.test.cpp
    market-data/format/feed_status_format.test.cpp
    market-data/format/market_data_format.test.cpp

    market-data/parser/fixed_point.test.cpp

    risk_management/gate/backpressure.test.cpp
    risk_management/gate/composition.test.cpp
    risk_management/gate/feedback.test.cpp
    risk_management/gate/loss_limit.test.cpp
    risk_management/gate/observer.test.cpp
    risk_management/gate/screening.test.cpp

    risk_management/hooks/breach.test.cpp
    risk_management/hooks/feedback.test.cpp
    risk_management/hooks/feedback_death.test.cpp

    risk_management/hooks/post_trade/fill_burst.test.cpp
    risk_management/hooks/post_trade/monitor.test.cpp
    risk_management/hooks/post_trade/order_trade_ratio.test.cpp
    risk_management/hooks/post_trade/outcome_silence.test.cpp
    risk_management/hooks/post_trade/routing.test.cpp
    risk_management/hooks/post_trade/routing_death.test.cpp

    risk_management/hooks/pre_trade/duplicate.test.cpp
    risk_management/hooks/pre_trade/order_size_check.test.cpp
    risk_management/hooks/pre_trade/position_book.test.cpp
    risk_management/hooks/pre_trade/position_limit.test.cpp
    risk_management/hooks/pre_trade/price_collar.test.cpp
    risk_management/hooks/pre_trade/rate_limiter.test.cpp
    risk_management/hooks/pre_trade/working_ledger.test.cpp

    risk_management/hooks/system/circuit_breaker.test.cpp
    risk_management/hooks/system/global_kill_switch.test.cpp
    risk_management/hooks/system/heartbeat.test.cpp
    risk_management/hooks/system/pnl_drawdown_breaker.test.cpp

    strategy/command_writer.test.cpp
    strategy/iceberg.test.cpp
    strategy/quoter.test.cpp
    strategy/stop.test.cpp
    strategy/strategy_engine.test.cpp

    strategy/backtest/clock.test.cpp
    strategy/backtest/depth_feed_bridge.test.cpp
    strategy/backtest/fill_model.test.cpp
    strategy/backtest/queue_position.test.cpp
    strategy/backtest/scheduler.test.cpp
    strategy/backtest/session.test.cpp
    strategy/backtest/wire.test.cpp

    trading-engine/event/command_format.test.cpp
    trading-engine/event/engine_event.test.cpp
    trading-engine/event/event_channel.test.cpp
    trading-engine/event/event_channel_death.test.cpp
    trading-engine/event/journal_record.test.cpp
    trading-engine/event/lifecycle.test.cpp

    trading-engine/event/event_dispatcher/backpressure.test.cpp
    trading-engine/event/event_dispatcher/handoff.test.cpp
    trading-engine/event/event_dispatcher/routing.test.cpp

    trading-engine/execution/book_manager.test.cpp
    trading-engine/execution/book_snapshot.test.cpp
    trading-engine/execution/dispatcher.test.cpp

    trading-engine/execution/engine_partition/cancel_reasons.test.cpp
    trading-engine/execution/engine_partition/drain.test.cpp
    trading-engine/execution/engine_partition/journal.test.cpp
    trading-engine/execution/engine_partition/listings.test.cpp
    trading-engine/execution/engine_partition/metrics.test.cpp
    trading-engine/execution/engine_partition/records.test.cpp
    trading-engine/execution/engine_partition/recovery.test.cpp
    trading-engine/execution/engine_partition/runs.test.cpp

    trading-engine/execution/order_manager/admit.test.cpp
    trading-engine/execution/order_manager/cancellable.test.cpp
    trading-engine/execution/order_manager/death.test.cpp
    trading-engine/execution/order_manager/lifecycle.test.cpp
    trading-engine/execution/order_manager/recycling.test.cpp

    trading-engine/execution/routing.test.cpp

    trading-engine/format/trading_engine_format.test.cpp

    trading-engine/order_book/cached_optimised_order_book.test.cpp
    trading-engine/order_book/order_book.test.cpp
    trading-engine/order_book/order_outcomes.test.cpp
    trading-engine/order_book/order_pool.test.cpp
    trading-engine/order_book/order_state.test.cpp
    trading-engine/order_book/order_state_death.test.cpp

    trading-engine/orders/order.test.cpp

    trading-engine/symbol/parse_exact_decimal.test.cpp
    trading-engine/symbol/symbol_registry.test.cpp
    trading-engine/symbol/symbol_spec.test.cpp
    trading-engine/symbol/validation.test.cpp

    transport/rest_failure.test.cpp
)

target_link_libraries(order_test PRIVATE
    exchange_engine::session
    exchange_engine::symbol
    exchange_engine::execution
    GTest::gtest
    GTest::gmock
    GTest::gtest_main
    Threads::Threads)

# Fixture headers are included by path relative to the test file that uses them,
# so the test tree itself has to be on the include path.
target_include_directories(order_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# --- Precompiled header ---------------------------------------------------
# Third-party headers only, and that restriction is the whole design: a PCH
# holding a project header is invalidated by every edit to that header, which is
# the opposite of what this is for.
#
# What is in it was chosen by measurement, not by guess. Every one of the 143
# TUs parses gtest, which is roughly the 2s floor a trivial suite costs; the
# ones in the 10-20s band all reach Boost.Intrusive and Boost.Pool through
# order_book.hpp -> price_level.hpp -> detail/, and boost::unordered_flat_map
# through execution/order_manager.hpp. Those four Boost headers are the single
# biggest shared cost in the suite and none of them is a project decision a test
# can avoid.
#
# Boost.Asio and Beast are deliberately absent: they are heavier still, but only
# about seven TUs include them, so putting them here would charge the other 136
# for something they never use.
target_precompile_headers(order_test PRIVATE
    <gtest/gtest.h>
    <gmock/gmock.h>
    <fmt/format.h>
    <fmt/ranges.h>
    <boost/pool/pool.hpp>
    <boost/intrusive/list.hpp>
    <boost/intrusive/set.hpp>
    <boost/unordered/unordered_flat_map.hpp>)

# ccache will not reuse an object built against a precompiled header unless it
# is told to be sloppy about the #defines the PCH baked in. Left alone, the PCH
# is a straight downgrade for the build everyone actually runs: cold builds get
# faster, and every incremental build turns from a cache hit into a full compile.
#
# Scoped to this target rather than set globally, and expressed through
# `cmake -E env` rather than by editing the developer's ccache.conf: the rest of
# the tree keeps the plain launcher, and nothing outside the build tree is
# modified by configuring this project.
if(CMAKE_CXX_COMPILER_LAUNCHER)
    set_target_properties(order_test PROPERTIES
        CXX_COMPILER_LAUNCHER
        "${CMAKE_COMMAND};-E;env;CCACHE_SLOPPINESS=pch_defines,time_macros;${CMAKE_CXX_COMPILER_LAUNCHER}")
endif()

# Header-only code (e.g. the hazard-pointer library) is compiled into this
# target, so hardening must be enabled here for its assert()s to survive an
# optimized build.
enable_hardening(order_test)

# The ASan runtime is the compiler's, not vcpkg's, so applocal deployment does
# not bring it along. Without this the AddressSanitizer build produces a binary
# that cannot start, and CTest reports it as a discovery failure.
copy_sanitizer_runtime(order_test)

# Register each GoogleTest case with CTest. PRE_TEST discovery runs the exe at
# ctest time (not build time), so a missing runtime DLL can't break the build.
include(GoogleTest)
gtest_discover_tests(order_test DISCOVERY_MODE PRE_TEST)
