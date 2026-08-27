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
    app/latency_pipe.test.cpp
    app/ledger_view.test.cpp
    app/live_session.test.cpp
    app/live_session_fills.test.cpp
    app/live_session_latency.test.cpp
    app/live_session_reaction.test.cpp
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
    core/persistence/manifest_format.test.cpp
    core/persistence/record_log.test.cpp
    core/persistence/replay.test.cpp

    core/chrono/feed_clock.test.cpp
    core/chrono/ingress_clock.test.cpp

    core/util/enum_conversion.test.cpp
    core/util/enum_parse.test.cpp
    core/util/flag.test.cpp
    core/util/function_ref.test.cpp
    core/util/format_as.test.cpp
    core/util/nested_formatter_contract.test.cpp

    market_data/binance/apply_depth_update.test.cpp
    market_data/binance/depth_snapshot_endpoint.test.cpp
    market_data/binance/depth_frame_decoder.test.cpp
    market_data/binance/depth_speed.test.cpp
    market_data/binance/diff_depth_stream.test.cpp
    market_data/binance/exchange_info_endpoint.test.cpp
    market_data/binance/jsonl_depth_feed.test.cpp
    market_data/binance/live_api.test.cpp
    market_data/binance/normalise.test.cpp
    market_data/binance/parse_api_error.test.cpp
    market_data/binance/parse_binance_depth.test.cpp
    market_data/binance/parse_depth_update.test.cpp
    market_data/binance/parse_depth_updates.test.cpp
    market_data/binance/parse_exchange_info.test.cpp
    market_data/binance/parse_scaled.test.cpp
    market_data/binance/significant_decimals.test.cpp

    market_data/book/l2_book.test.cpp
    market_data/book/l2_book_sweep.test.cpp
    market_data/book/load_side.test.cpp

    market_data/feed/apply_event.test.cpp
    market_data/feed/depth_reconstructor.test.cpp
    market_data/feed/depth_sequencer.test.cpp
    market_data/feed/drive.test.cpp
    market_data/feed/reconstructor_ingress.test.cpp
    market_data/feed/replay_feed.test.cpp
    market_data/feed/reset_book.test.cpp

    market_data/format/depth_parse_error_format.test.cpp
    market_data/format/feed_status_format.test.cpp
    market_data/format/market_data_format.test.cpp

    market_data/parser/fixed_point.test.cpp

    risk_management/window.test.cpp

    risk_management/gate/risk_gate_backpressure.test.cpp
    risk_management/gate/composition.test.cpp
    risk_management/gate/risk_gate_feedback.test.cpp
    risk_management/gate/loss_limit.test.cpp
    risk_management/gate/observer.test.cpp
    risk_management/gate/screening.test.cpp

    risk_management/hooks/breach.test.cpp
    risk_management/hooks/risk_hooks_feedback.test.cpp
    risk_management/hooks/feedback_death.test.cpp

    risk_management/hooks/post_trade/fill_burst.test.cpp
    risk_management/hooks/post_trade/monitor.test.cpp
    risk_management/hooks/post_trade/order_trade_ratio.test.cpp
    risk_management/hooks/post_trade/outcome_silence.test.cpp
    risk_management/hooks/post_trade/post_trade_routing.test.cpp
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

    strategy/backtest/depth_feed_bridge.test.cpp
    strategy/backtest/fill_model.test.cpp
    strategy/backtest/backtest_queue_position.test.cpp
    strategy/backtest/scheduler.test.cpp
    strategy/backtest/session.test.cpp
    strategy/backtest/wire.test.cpp

    event/command_format.test.cpp
    event/engine_event.test.cpp
    event/event_channel.test.cpp
    event/event_channel_death.test.cpp
    event/journal_record.test.cpp
    event/event_lifecycle.test.cpp

    event/event_dispatcher/event_dispatcher_backpressure.test.cpp
    event/event_dispatcher/handoff.test.cpp
    event/event_dispatcher/event_dispatcher_routing.test.cpp

    execution/book_manager.test.cpp
    execution/book_snapshot.test.cpp
    execution/dispatcher.test.cpp

    execution/engine_partition/cancel_reasons.test.cpp
    execution/engine_partition/drain.test.cpp
    execution/engine_partition/journal.test.cpp
    execution/engine_partition/listings.test.cpp
    execution/engine_partition/metrics.test.cpp
    execution/engine_partition/records.test.cpp
    execution/engine_partition/recovery.test.cpp
    execution/engine_partition/runs.test.cpp

    execution/order_manager/admit.test.cpp
    execution/order_manager/cancellable.test.cpp
    execution/order_manager/death.test.cpp
    execution/order_manager/order_manager_lifecycle.test.cpp
    execution/order_manager/recycling.test.cpp

    execution/execution_routing.test.cpp

    event/event_lifecycle_format.test.cpp
    execution/execution_format.test.cpp
    order_book/order_book_format.test.cpp
    orders/orders_format.test.cpp

    order_book/allocation.test.cpp
    order_book/cached_optimised_order_book.test.cpp
    order_book/order_book.test.cpp
    order_book/order_outcomes.test.cpp
    order_book/order_pool.test.cpp
    order_book/order_state.test.cpp
    order_book/order_state_death.test.cpp
    order_book/order_book_queue_position.test.cpp
    order_book/sweep_estimate.test.cpp

    orders/order.test.cpp

    symbol/parse_exact_decimal.test.cpp
    symbol/symbol_registry.test.cpp
    symbol/symbol_spec.test.cpp
    symbol/validation.test.cpp

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

# Unity batches by default, a precompiled header when they are turned off.
# Never both, and never unity under clang-tidy. @see testing.md
if(ORDER_BOOK_ENABLE_UNITY_BUILD AND NOT CMAKE_CXX_CLANG_TIDY)
    set_target_properties(order_test PROPERTIES
        UNITY_BUILD ON
        UNITY_BUILD_MODE BATCH
        UNITY_BUILD_BATCH_SIZE 16)
elseif(ORDER_BOOK_ENABLE_PCH)
    set_target_properties(order_test PROPERTIES UNITY_BUILD OFF)
    target_precompile_headers(order_test PRIVATE
        <gtest/gtest.h>
        <boost/pool/pool.hpp>
        <boost/intrusive/list.hpp>
        <boost/intrusive/set.hpp>
        <boost/unordered/unordered_flat_map.hpp>)

    if(CMAKE_CXX_COMPILER_LAUNCHER)
        set_target_properties(order_test PROPERTIES
            CXX_COMPILER_LAUNCHER
            "${CMAKE_COMMAND};-E;env;CCACHE_SLOPPINESS=pch_defines,time_macros;${CMAKE_CXX_COMPILER_LAUNCHER}")
    endif()
else()
    set_target_properties(order_test PROPERTIES UNITY_BUILD OFF)
endif()

# Header-only code (e.g. the hazard-pointer library) is compiled into this
# target, so hardening must be enabled here for its assert()s to survive an
# optimized build.
set_warnings(order_test)

# gtest's EXPECT_*/ASSERT_* expand to `switch (0) case 0: default: if (...)
# else ...`, which is what makes a trailing user `else` bind correctly - but GCC
# reads an unbraced `if (cond) EXPECT_EQ(...)` as a dangling else anyway. The
# braces it asks for are removed again by RemoveBracesLLVM in .clang-format, so
# only one of the two can be satisfied and the macro is the one that is right.
target_compile_options(order_test
    PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wno-dangling-else>)

enable_hardening(order_test)
enable_coverage(order_test)

# The ASan runtime is the compiler's, not vcpkg's, so applocal deployment does
# not bring it along. Without this the AddressSanitizer build produces a binary
# that cannot start, and CTest reports it as a discovery failure.
copy_sanitizer_runtime(order_test)

# Register each GoogleTest case with CTest. PRE_TEST discovery runs the exe at
# ctest time (not build time), so a missing runtime DLL can't break the build.
include(GoogleTest)
gtest_discover_tests(order_test DISCOVERY_MODE PRE_TEST)
