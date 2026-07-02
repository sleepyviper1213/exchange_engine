# AFL++ fuzz harnesses. Built only when ORDERBOOK_ENABLE_FUZZING is ON (see the
# top-level CMakeLists). To get coverage instrumentation, configure with an AFL++
# compiler; without one the harnesses still build and fall back to reading a
# single testcase from stdin (crash replay). See README.md for the full workflow.
#
#   CC=afl-clang-fast CXX=afl-clang-fast++ \
#     cmake -S . -B build/fuzz -DORDERBOOK_ENABLE_FUZZING=ON -DCMAKE_BUILD_TYPE=Release

add_executable(parse_depth_fuzz parse_depth_fuzz.cpp)
target_link_libraries(parse_depth_fuzz PRIVATE order_book fmt::fmt)

add_executable(engine_ops_fuzz engine_ops_fuzz.cpp)
target_link_libraries(engine_ops_fuzz PRIVATE order_book fmt::fmt)
