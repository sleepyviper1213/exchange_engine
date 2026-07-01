add_executable(order_bench order_book.cpp snapshot_load.cpp market_replay.cpp)
target_link_libraries(order_bench PRIVATE order_book benchmark::benchmark benchmark::benchmark_main)
# Benchmarks are meaningless at the default -O0, so force -O2 for GCC/Clang in
# every config. MSVC is deliberately excluded: /O2 conflicts with Debug's /RTC1
# (error D8016), and MSVC's Release/RelWithDebInfo/Sanitize configs already imply
# /O2 — so build the benchmark in one of those configs on MSVC. A Debug build
# stays unoptimized (and would report Debug timings anyway).
target_compile_options(order_bench PRIVATE
        $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-O2>)
# Runtime DLLs are already in the shared output dir; only the ASan runtime DLL
# needs staging for a Sanitize-config benchmark.
#order_book_copy_asan_runtime(order_bench)
