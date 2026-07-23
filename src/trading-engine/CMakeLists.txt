add_library(trading_engine SHARED
    order_book/level.cpp
    order_book/order.cpp
    order_book/resting_order.cpp
    order_book/book_side.cpp
    order_book/order_book.cpp
    order_book/order_list.cpp
    event/command.cpp
)
add_library(exchange_engine::trading_engine ALIAS trading_engine)

target_include_directories(trading_engine
    PUBLIC
    ${EE_INCLUDE_ROOTS}
    ${CMAKE_CURRENT_BINARY_DIR}   # generated trading_engine_export.h
)

target_link_libraries(trading_engine
    PUBLIC
    fmt::fmt
)

include(GenerateExportHeader)
generate_export_header(trading_engine)

# Exported classes hold std:: members/bases (unordered_map, vector) — C4251/C4275
# are ABI-pedantry that don't apply to same-toolchain internal libs.
target_compile_options(trading_engine PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/wd4251>
    $<$<CXX_COMPILER_ID:MSVC>:/wd4275>
)

enable_coverage(trading_engine)
set_warnings(trading_engine)
enable_hardening(trading_engine)