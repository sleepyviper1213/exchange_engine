add_library(market_data SHARED
    binance/binance_depth.cpp
    binance/endpoints.cpp
    parser/fixed_point.cpp
    l2_book.cpp
)

add_library(exchange_engine::market_data ALIAS market_data)

target_include_directories(market_data
    PUBLIC
	PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
           $<INSTALL_INTERFACE:include>)

target_link_libraries(market_data
    PUBLIC
    exchange_engine::core
    fmt::fmt
	simdjson::simdjson
)

if (WIN32)
    target_compile_definitions(market_data PUBLIC _WIN32_WINNT=0x0601)
endif ()

target_compile_options(market_data PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/wd4251>
    $<$<CXX_COMPILER_ID:MSVC>:/wd4275>
)

generate_module_export_header(market_data)
enable_coverage(market_data)
set_warnings(market_data)
enable_hardening(market_data)
