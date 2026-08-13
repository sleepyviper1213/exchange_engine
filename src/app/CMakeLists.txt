add_executable(exchange_tool
    cli.cpp
    configuration.cpp
    main.cpp)

target_link_libraries(exchange_tool
    PRIVATE
    exchange_engine::transport
    exchange_engine::market_data
    exchange_engine::trading_engine
    exchange_engine::risk_management
    exchange_engine::strategy
    exchange_engine::core
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
