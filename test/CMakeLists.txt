add_executable(order_test
    order_book.test.cpp
    branchless_lower_bound.test.cpp
        io/binance_depth_test.cpp
        concurrent_queue/fast_queue_test.cpp
        concurrent_queue/spsc_queue_test.cpp
        concurrent_queue/spsc_queue_non_pod_test.cpp
    io/binance_depth_test.cpp
    concurrent_queue/fast_queue_test.cpp
    concurrent_queue/spsc_queue_test.cpp
    concurrent_queue/spsc_queue_non_pod_test.cpp
    concurrent_queue/spsc_queue_concurrency_test.cpp
    lockfree/hashmap_test.cpp order.test.cpp)
target_link_libraries(order_test PRIVATE
    order_book::market_data
    GTest::gtest
    GTest::gtest_main
    Threads::Threads)

# Header-only code (e.g. the hazard-pointer library) is compiled into this
# target, so hardening must be enabled here for its assert()s to survive an
# optimized build.
enable_hardening(order_test)

# Register each GoogleTest case with CTest. PRE_TEST discovery runs the exe at
# ctest time (not build time), so a missing runtime DLL can't break the build.
include(GoogleTest)
gtest_discover_tests(order_test DISCOVERY_MODE PRE_TEST)