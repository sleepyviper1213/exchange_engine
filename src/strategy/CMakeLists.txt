add_library(strategy SHARED
    command_writer.cpp

    backtest/depth_feed_bridge.cpp
    backtest/queue_position.cpp
)
add_library(exchange_engine::strategy ALIAS strategy)

target_include_directories(strategy
    PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(strategy
    PUBLIC
    exchange_engine::risk_management
    exchange_engine::market_data
    exchange_engine::symbol
    exchange_engine::core
)

target_compile_options(strategy PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/wd4251 /wd4275>
)

generate_module_export_header(strategy)
enable_coverage(strategy)
set_warnings(strategy)
enable_hardening(strategy)
