# market-data — namespace market_data. Binance depth parsing (market_data::binance)
# plus the live capture/snapshot tools. Depends on core for engine::Order/OrderBook.
add_library(market_data SHARED
    binance/binance_depth.cpp
)
add_library(order_book::market_data ALIAS market_data)

target_include_directories(market_data
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(market_data
    PUBLIC
        order_book::core
        simdjson::simdjson
        fmt::fmt
)

if(WIN32)
    set_target_properties(market_data
        PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
    target_compile_definitions(market_data PUBLIC _WIN32_WINNT=0x0601)
endif()

enable_coverage(market_data)
set_warnings(market_data)
enable_hardening(market_data)

# HTTPS/WebSocket tools: sockets + TLS on top of the market_data library.
function(add_tool target source)
    add_executable(${target} ${source})

    target_link_libraries(${target}
        PRIVATE
            order_book::market_data
            OpenSSL::SSL
            OpenSSL::Crypto
    )

    if(boost_beast_FOUND)
        target_link_libraries(${target} PRIVATE Boost::beast)
    else()
        target_link_libraries(${target} PRIVATE Boost::thread)
    endif()

    if(WIN32)
        target_link_libraries(${target} PRIVATE ws2_32 crypt32)
    endif()
endfunction()

add_tool(snapshot_tool tools/snapshot_main.cpp)
add_tool(ws_capture_tool tools/ws_capture_main.cpp)
