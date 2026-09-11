add_library(venue SHARED
    binance/api_error.cpp
    binance/exchange_info.cpp
    binance/order.cpp
    binance/signing.cpp
    binance/user_data.cpp
    credentials.cpp
    weight_budget.cpp
)

add_library(exchange_engine::venue ALIAS venue)

target_include_directories(venue
	PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
           $<INSTALL_INTERFACE:include>)

target_link_libraries(venue
    PUBLIC
    exchange_engine::orders
    exchange_engine::core
    fmt::fmt
    PRIVATE
    simdjson::simdjson
    OpenSSL::Crypto
)

generate_module_export_header(venue)
enable_coverage(venue)
set_warnings(venue)
enable_hardening(venue)
