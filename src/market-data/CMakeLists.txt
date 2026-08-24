add_library(market_data SHARED
    binance/api_error.cpp
    binance/binance_depth.cpp
    binance/depth_feed.cpp
    binance/endpoints.cpp
    binance/exchange_info.cpp
    binance/normalise.cpp
    parser/fixed_point.cpp
    l2_book.cpp
    normalised.cpp
    reconstructor.cpp
    sequencer.cpp
)

add_library(exchange_engine::market_data ALIAS market_data)

target_include_directories(market_data
	PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
           $<INSTALL_INTERFACE:include>)

# `orders` is the engine's *vocabulary* - price_t, quantity_t, side_t - and
# l2_book.hpp, normalised.hpp and normalised.cpp have always included it. Until
# the engine was split there was no way to say so: naming a target meant naming
# `trading_engine`, and market-data and trading-engine are siblings that must
# never link each other. So the dependency went undeclared and compiled by luck,
# on `core` happening to export src/ as an include directory.
#
# Now the vocabulary is its own module *below* both siblings, so this is an edge
# downward like every other rather than one across. @see docs/directory_layout.md
target_link_libraries(market_data
    PUBLIC
    exchange_engine::orders
    exchange_engine::core
    fmt::fmt
	simdjson::simdjson
)


generate_module_export_header(market_data)
enable_coverage(market_data)
set_warnings(market_data)
enable_hardening(market_data)
