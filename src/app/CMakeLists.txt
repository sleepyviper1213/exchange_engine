add_executable(exchange_tool
    cli.cpp
    configuration.cpp
    main.cpp

    commands/backtest.cpp
    commands/capture.cpp
    commands/demo.cpp
    commands/live.cpp
    commands/recover.cpp
    commands/replay.cpp
    commands/serve.cpp
    commands/snapshot.cpp)

target_link_libraries(exchange_tool
	PUBLIC
	exchange_engine::execution
	exchange_engine::symbol
	exchange_engine::session
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
