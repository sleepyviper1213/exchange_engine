add_executable(order_test
    order_book.test.cpp
    l2_book.test.cpp
    branchless_lower_bound.test.cpp
    binance_endpoints.test.cpp
    format.test.cpp
    normalise.test.cpp
    reconstructor.test.cpp
    sequencer.test.cpp
    io/binance_depth_test.cpp
    io/fixed_point_test.cpp
    matching_engine_test.cpp
    order.test.cpp
    order_state.test.cpp
    order_outcome.test.cpp
    symbol_spec.test.cpp
    allocator/object_pool_test.cpp
    allocator/node_pool_test.cpp
    allocator/slab_test.cpp
    allocator/allocator_test.cpp
    allocator/free_list_test.cpp
    allocator/freelist_test.cpp
    concurrency/affinity/core_allocator_test.cpp
    concurrency/synchronisation/hazard_pointer_test.cpp
    concurrency/lockfree/queue/fast_queue_test.cpp
    concurrency/lockfree/queue/spsc_queue_test.cpp
    concurrency/lockfree/queue/spsc_queue_non_pod_test.cpp
    concurrency/lockfree/queue/spsc_queue_concurrency_test.cpp
    concurrency/lockfree/hashmap_test.cpp
    concurrency/lockfree/stack_test.cpp
)

target_link_libraries(order_test PRIVATE
    exchange_engine::market_data
    exchange_engine::trading_engine
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
