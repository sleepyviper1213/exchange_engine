# The benchmark tree mirrors src/ the same way test/ does, and every file holds
# one benchmark family:
#
#     <module>/<group>/<subject>.bench.cpp
#     <module>/<group>/<component>/<aspect>.bench.cpp
#

add_executable(exchange_bench
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

    core/optimisation/false_sharing.bench.cpp

    core/scaled/fixed_point.bench.cpp

    market_data/feed/reconstructor.bench.cpp

    execution/engine_partition_metrics.bench.cpp
    execution/engine_partition_journal.bench.cpp
    execution/matching_engine.bench.cpp
    execution/order_manager.bench.cpp

    risk_management/gate.bench.cpp
    risk_management/latency.bench.cpp

    order_book/cached_optimised_order_book.bench.cpp
    order_book/order_book.bench.cpp
    order_book/snapshot_load.bench.cpp
)

target_link_libraries(exchange_bench 
	PRIVATE
    exchange_engine::session
    exchange_engine::execution
    exchange_engine::market_data
    benchmark::benchmark
)

target_include_directories(exchange_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Vendored comparison queues, kept verbatim so the benchmark measures what those
# projects ship. SYSTEM silences the warnings we would otherwise have to fix in
# code that is not ours.
target_include_directories(exchange_bench SYSTEM PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/core/concurrency/lockfree/queue)

copy_sanitizer_runtime(exchange_bench)

set_target_properties(exchange_bench PROPERTIES UNITY_BUILD OFF)

set_warnings(exchange_bench)

add_executable(exchange::benchmark ALIAS exchange_bench)
