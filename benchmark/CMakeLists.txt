add_executable(order_bench order_book.cpp snapshot_load.cpp market_replay.cpp
        matching_engine.cpp
        object_pool.cpp
        concurrent_queue/spsc_queue.cpp
        concurrent_queue/llc_placement.cpp
        concurrent_queue/priority_compare.cpp
        concurrent_queue/fifo5b.cpp
        concurrent_queue/reader_writer_queue.cpp
        concurrent_queue/producer_consumer_queue.cpp
        watchdog.cpp
  )
target_link_libraries(order_bench PRIVATE
    exchange_engine::market_data
    benchmark::benchmark
    benchmark::benchmark_main)
add_executable(order_book::benchmark ALIAS order_bench)
