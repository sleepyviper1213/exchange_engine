add_executable(order_test order_book.test.cpp branchless_lower_bound.test.cpp
        io/binance_depth_test.cpp
        concurrent_queue/fast_queue_test.cpp
        concurrent_queue/spsc_queue_test.cpp
        concurrent_queue/spsc_queue_non_pod_test.cpp order.test.cpp)
target_link_libraries(order_test PRIVATE order_book concurrent_queue
        GTest::gtest GTest::gtest_main Threads::Threads)

# order_book.dll and the vcpkg DLLs are already in the shared output dir (see the
# top-level CMAKE_RUNTIME_OUTPUT_DIRECTORY + vcpkg applocal). Only the ASan
# runtime DLL still needs staging for the AddressSanitizer config.
#order_book_copy_asan_runtime(order_test)

# Register each GoogleTest case with CTest. PRE_TEST discovery runs the exe at
# ctest time (not build time), so a missing runtime DLL can't break the build.
include(GoogleTest)
gtest_discover_tests(order_test DISCOVERY_MODE PRE_TEST)