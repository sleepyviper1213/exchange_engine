add_executable(exchange_tool
    configuration.cpp
    main.cpp

    cli/account_options.cpp
    cli/backtest_options.cpp
    cli/capture_options.cpp
    cli/demo_options.cpp
    cli/live_options.cpp
    cli/recover_options.cpp
    cli/replay_options.cpp
    cli/serve_options.cpp
    cli/snapshot_options.cpp
    cli/trades_options.cpp

    commands/account.cpp
    commands/backtest.cpp
    commands/capture.cpp
    commands/demo.cpp
    commands/live.cpp
    commands/recover.cpp
    commands/replay.cpp
    commands/seed.cpp
    commands/serve.cpp
    commands/snapshot.cpp
    commands/trades.cpp
    commands/venue_orders.cpp)

target_link_libraries(exchange_tool
	PUBLIC
	exchange_engine::execution
	exchange_engine::symbol
    exchange_engine::venue
    exchange_engine::market_data
	exchange_engine::session
    exchange_engine::strategy
    Threads::Threads
    CLI11::CLI11
    spdlog::spdlog
    exchange_engine::malloc
)

target_include_directories(exchange_tool 
	PRIVATE 
	"${CMAKE_BINARY_DIR}/configured_files/include"
	$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)

set_warnings(exchange_tool)
