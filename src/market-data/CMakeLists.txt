add_library(market_data SHARED
    binance/binance_depth.cpp
    binance/endpoints.cpp
    binance/normalise.cpp
    parser/fixed_point.cpp
    l2_book.cpp
    normalised.cpp
    reconstructor.cpp
    sequencer.cpp
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


generate_module_export_header(market_data)
enable_coverage(market_data)
set_warnings(market_data)
enable_hardening(market_data)
