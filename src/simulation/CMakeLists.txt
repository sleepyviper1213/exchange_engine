add_library(simulation SHARED
    depth_feed_bridge.cpp
    queue_position.cpp
)
add_library(exchange_engine::simulation ALIAS simulation)

target_include_directories(simulation
    PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(simulation
    PUBLIC
    exchange_engine::event
    exchange_engine::market_data
    exchange_engine::symbol
    exchange_engine::core
)

target_compile_options(simulation PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/wd4251 /wd4275>
)

generate_module_export_header(simulation)
enable_coverage(simulation)
set_warnings(simulation)
enable_hardening(simulation)
