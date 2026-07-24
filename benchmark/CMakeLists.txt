add_executable(order_bench
    order_book.cpp
    snapshot_load.cpp
    market_replay.cpp
    matching_engine.cpp
    object_pool.cpp
    memory/free_list.cpp
    concurrency/lockfree/queue/spsc_queue.cpp
    concurrency/lockfree/queue/llc_placement.cpp
    concurrency/lockfree/queue/priority_compare.cpp
    concurrency/lockfree/queue/fifo5b.cpp
    concurrency/lockfree/queue/reader_writer_queue.cpp
    concurrency/lockfree/queue/producer_consumer_queue.cpp
    memory/freelist.cpp
)

target_link_libraries(order_bench PRIVATE
    exchange_engine::market_data
    benchmark::benchmark
    benchmark::benchmark_main
)

add_executable(order_book::benchmark ALIAS order_bench)
