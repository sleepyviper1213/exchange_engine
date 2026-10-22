add_library(strategy_backtest SHARED
    markout_recorder.cpp
    tape_audit.cpp
)
add_library(exchange_engine::strategy_backtest ALIAS strategy_backtest)

target_include_directories(strategy_backtest
    PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(strategy_backtest
    PUBLIC
    exchange_engine::strategy
    exchange_engine::simulation
    exchange_engine::risk_management
    exchange_engine::execution
)

target_compile_options(strategy_backtest PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/wd4251 /wd4275>
)

generate_module_export_header(strategy_backtest)
enable_coverage(strategy_backtest)
set_warnings(strategy_backtest)
enable_hardening(strategy_backtest)
