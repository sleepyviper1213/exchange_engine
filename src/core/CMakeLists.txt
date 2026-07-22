add_library(core SHARED
    engine/level.cpp
    #    engine/order_list.cpp
    engine/order.cpp
    engine/resting_order.cpp
    engine/book_side.cpp
    engine/order_book.cpp
        engine/level.cpp
        engine/order_list.cpp
        engine/order.cpp
        engine/resting_order.cpp
        engine/book_side.cpp
        engine/order_book.cpp
)
add_library(order_book::core ALIAS core)

target_include_directories(core
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(core
    PUBLIC
    fmt::fmt
)

target_compile_options(core
    PUBLIC
    $<$<CXX_COMPILER_ID:GNU>:-Wa,-mbig-obj>
    $<$<CXX_COMPILER_ID:MSVC>:/bigobj>
)

if (WIN32)
    set_target_properties(core
        PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
    target_compile_definitions(core PUBLIC _WIN32_WINNT=0x0601)
endif ()

enable_coverage(core)
set_warnings(core)

# End-to-end MatchingEngine driver. Needs only the library + threads (no sockets).
add_executable(engine_demo_tool tools/engine_demo_main.cpp)
target_link_libraries(engine_demo_tool PRIVATE order_book::core Threads::Threads)
