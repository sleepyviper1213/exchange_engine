# The benchmark tree mirrors src/ the same way test/ does, and every file holds
# one benchmark family:
#
#     <module>/<group>/<subject>.bench.cpp
#     <module>/<group>/<component>/<aspect>.bench.cpp
#
# So the path says which module a number belongs to before you open anything,
# and a `--benchmark_filter` result can be traced back to a source file by name.
# Helpers shared by sibling benchmarks live in a `<component>.fixture.hpp` next
# to them rather than being copied — the same rule the test tree follows.
#
# A cross-module benchmark goes under app/, because app/ is where the dependency
# graph allows market-data and trading-engine to be named together: market_replay
# runs a venue L2 feed against both books, so it lives beside the join it
# measures, not inside either subsystem.
#
# One binary rather than one per subsystem, matching order_test: filters select
# what runs, and a single target keeps the link configuration in one place.
# Sources are grouped below in src/ order.

add_executable(order_bench
    main.cpp

    app/market_replay.bench.cpp

    core/concurrency/lockfree/queue/fifo5b.bench.cpp
    core/concurrency/lockfree/queue/llc_placement.bench.cpp
    core/concurrency/lockfree/queue/priority_compare.bench.cpp
    core/concurrency/lockfree/queue/producer_consumer_queue.bench.cpp
    core/concurrency/lockfree/queue/reader_writer_queue.bench.cpp
    core/concurrency/lockfree/queue/spsc_queue.bench.cpp

    core/memory/object_pool.bench.cpp

    core/metrics/metrics.bench.cpp

    market-data/feed/reconstructor.bench.cpp

    market-data/parser/fixed_point.bench.cpp

    trading-engine/execution/engine_partition_metrics.bench.cpp
    trading-engine/execution/matching_engine.bench.cpp
    trading-engine/execution/order_manager.bench.cpp

    risk_management/gate.bench.cpp
    risk_management/latency.bench.cpp

    trading-engine/order_book/cached_optimised_order_book.bench.cpp
    trading-engine/order_book/order_book.bench.cpp
    trading-engine/order_book/snapshot_load.bench.cpp
)

target_link_libraries(order_bench PRIVATE
    exchange_engine::core
    exchange_engine::market_data
    exchange_engine::trading_engine
    exchange_engine::risk_management
    benchmark::benchmark
)

target_include_directories(order_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

copy_sanitizer_runtime(order_bench)

add_executable(order_book::benchmark ALIAS order_bench)
