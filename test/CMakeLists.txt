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
    app/depth_feed_bridge.test.cpp

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

    core/memory/allocator.test.cpp
    core/memory/node_pool.test.cpp
    core/memory/object_pool.test.cpp
    core/memory/slab.test.cpp

    core/metrics/counter.test.cpp
    core/metrics/histogram.test.cpp
    core/metrics/histogram_format.test.cpp
    core/metrics/registry.test.cpp
    core/metrics/registry_format.test.cpp
    core/metrics/text_exposition.test.cpp

    core/optimisation/branchless_lower_bound.test.cpp

    core/util/enum_conversion.test.cpp
    core/util/flag.test.cpp
    core/util/format_as.test.cpp
    core/util/nested_formatter_contract.test.cpp

    market-data/binance/apply_depth_update.test.cpp
    market-data/binance/depth_snapshot_endpoint.test.cpp
    market-data/binance/depth_speed.test.cpp
    market-data/binance/diff_depth_stream.test.cpp
    market-data/binance/normalise.test.cpp
    market-data/binance/parse_binance_depth.test.cpp
    market-data/binance/parse_depth_update.test.cpp
    market-data/binance/parse_depth_updates.test.cpp
    market-data/binance/parse_scaled.test.cpp

    market-data/book/l2_book.test.cpp
    market-data/book/load_side.test.cpp

    market-data/feed/apply_event.test.cpp
    market-data/feed/depth_reconstructor.test.cpp
    market-data/feed/depth_sequencer.test.cpp
    market-data/feed/reset_book.test.cpp

    market-data/format/depth_parse_error_format.test.cpp
    market-data/format/market_data_format.test.cpp

    market-data/parser/fixed_point.test.cpp

    trading-engine/execution/book_manager.test.cpp
    trading-engine/execution/dispatcher.test.cpp
    
    trading-engine/execution/engine_partition/cancel_reasons.test.cpp
    trading-engine/execution/engine_partition/drain.test.cpp
    trading-engine/execution/engine_partition/listings.test.cpp
    trading-engine/execution/engine_partition/metrics.test.cpp
    trading-engine/execution/engine_partition/records.test.cpp

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

    trading-engine/risk/breach.test.cpp
    trading-engine/risk/circuit_breaker.test.cpp
    trading-engine/risk/position_book.test.cpp
    trading-engine/risk/rate_limiter.test.cpp
    trading-engine/risk/working_ledger.test.cpp
    trading-engine/risk/gate/backpressure.test.cpp
    trading-engine/risk/gate/composition.test.cpp
    trading-engine/risk/gate/feedback.test.cpp
    trading-engine/risk/gate/loss_limit.test.cpp
    trading-engine/risk/gate/screening.test.cpp

    trading-engine/strategy/command_writer.test.cpp
    trading-engine/strategy/iceberg.test.cpp
    trading-engine/strategy/stop.test.cpp
    trading-engine/strategy/strategy_engine.test.cpp

    trading-engine/symbol/parse_exact_decimal.test.cpp
    trading-engine/symbol/symbol_registry.test.cpp
    trading-engine/symbol/symbol_spec.test.cpp
    trading-engine/symbol/validation.test.cpp
)

target_link_libraries(order_test PRIVATE
    exchange_engine::market_data
    exchange_engine::trading_engine
    GTest::gtest
    GTest::gmock
    GTest::gtest_main
    Threads::Threads)

# Fixture headers are included by path relative to the test file that uses them,
# so the test tree itself has to be on the include path.
target_include_directories(order_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

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
