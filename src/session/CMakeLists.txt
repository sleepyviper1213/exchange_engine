# The live pipeline: what turns the modules below into a running venue.

add_library(session SHARED
	engine_logger.cpp
	gate_logger.cpp
)

add_library(exchange_engine::session ALIAS session)

target_include_directories(session
	PUBLIC
	$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
	$<INSTALL_INTERFACE:include>
)

target_link_libraries(session
	PUBLIC
	exchange_engine::core
	exchange_engine::market_data
	exchange_engine::trading_engine
	exchange_engine::risk_management
	exchange_engine::strategy
	exchange_engine::transport
)

generate_module_export_header(session)
enable_coverage(session)
set_warnings(session)
enable_hardening(session)
