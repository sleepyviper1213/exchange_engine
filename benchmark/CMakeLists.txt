add_executable(order_bench order_book.cpp snapshot_load.cpp market_replay.cpp
        concurrent_queue/spsc_queue.cpp)
target_link_libraries(order_bench PRIVATE order_book benchmark::benchmark benchmark::benchmark_main)
