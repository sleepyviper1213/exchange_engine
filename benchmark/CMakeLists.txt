add_executable(order_bench
    main.cpp
    order_book.cpp
    cached_optimised_order_book.cpp
    parse_fixed_point.cpp
    snapshot_load.cpp
    market_replay.cpp
    reconstructor.cpp
    matching_engine.cpp
    object_pool.cpp
    concurrency/lockfree/queue/spsc_queue.cpp
    concurrency/lockfree/queue/llc_placement.cpp
    concurrency/lockfree/queue/priority_compare.cpp
    concurrency/lockfree/queue/fifo5b.cpp
    concurrency/lockfree/queue/reader_writer_queue.cpp
    concurrency/lockfree/queue/producer_consumer_queue.cpp
)

target_link_libraries(order_bench PRIVATE
    exchange_engine::core
    exchange_engine::market_data
    exchange_engine::trading_engine
    benchmark::benchmark
)

add_executable(order_book::benchmark ALIAS order_bench)
