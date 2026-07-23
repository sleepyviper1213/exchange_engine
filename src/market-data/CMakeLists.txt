add_library(market_data SHARED
    binance/binance_depth.cpp
)
add_library(exchange_engine::market_data ALIAS market_data)

target_include_directories(market_data
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${EE_INCLUDE_ROOTS}
    ${CMAKE_CURRENT_BINARY_DIR}   # generated market_data_export.h
)

target_link_libraries(market_data
    PUBLIC
    exchange_engine::trading_engine
    exchange_engine::util
    simdjson::simdjson
    fmt::fmt
)

if (WIN32)
    target_compile_definitions(market_data PUBLIC _WIN32_WINNT=0x0601)
endif ()

# Explicit dllexport/dllimport via MARKET_DATA_EXPORT (works under MSVC ASan,
# unlike WINDOWS_EXPORT_ALL_SYMBOLS). Public API is annotated in binance_depth.hpp.
include(GenerateExportHeader)
generate_export_header(market_data)
target_compile_options(market_data PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/wd4251>
    $<$<CXX_COMPILER_ID:MSVC>:/wd4275>
)

enable_coverage(market_data)
set_warnings(market_data)
enable_hardening(market_data)