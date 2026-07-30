add_executable(exchange_tool
    cli.cpp
    configuration.cpp
    logger.cpp
    main.cpp)

# cli.cpp drives the matching engine, so trading_engine is a direct dependency.
# It is not reachable through transport or market_data — those are siblings, not
# dependents — and without it this target misses the generated
# trading_engine_export.hpp that the library puts on its PUBLIC include path.
target_link_libraries(exchange_tool
    PRIVATE
    exchange_engine::transport
    exchange_engine::market_data
    exchange_engine::trading_engine
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
