add_library(order_book SHARED
        engine/order_book.cpp
        io/binance_depth.cpp)
target_include_directories(order_book PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# MinGW/GCC and Clang auto-export all symbols from a shared library, so an import
# library is always produced. MSVC exports nothing without __declspec(dllexport)
# and then emits a DLL with no .lib, breaking every consumer (tests, benchmark,
# snapshot_tool) with LNK1104. Auto-export all symbols to match the other
# toolchains. Ignored on non-Windows platforms.
set_target_properties(order_book PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
add_subdirectory(concurrent_queue)

target_link_libraries(order_book PUBLIC fmt::fmt simdjson::simdjson)
# These TUs emit large object files. MinGW/GCC needs the -Wa,-mbig-obj assembler
# flag; MSVC needs its own /bigobj. Clang/AppleClang (macOS) needs neither and
# rejects both, so each is guarded to its compiler.
target_compile_options(order_book PUBLIC
        $<$<CXX_COMPILER_ID:GNU>:-Wa,-mbig-obj>
        $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)



add_executable(snapshot_tool tools/snapshot_main.cpp)
target_link_libraries(snapshot_tool PRIVATE order_book
        Boost::boost OpenSSL::SSL OpenSSL::Crypto)
if (WIN32)
    target_link_libraries(snapshot_tool PRIVATE ws2_32 crypt32)
endif ()

# Records a live Binance diff-depth WebSocket stream to a JSONL file that the
# market-replay benchmark replays offline. Same TLS deps as snapshot_tool; the
# WebSocket layer is header-only within Boost.Beast.
add_executable(ws_capture_tool tools/ws_capture_main.cpp)
target_link_libraries(ws_capture_tool PRIVATE order_book
        Boost::boost OpenSSL::SSL OpenSSL::Crypto)
if (WIN32)
    target_link_libraries(ws_capture_tool PRIVATE ws2_32 crypt32)
endif ()

#order_book_copy_asan_runtime(ws_capture_tool)
