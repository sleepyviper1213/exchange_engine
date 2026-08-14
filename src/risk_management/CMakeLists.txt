add_library(risk_management SHARED
	detail/position_entry.cpp
	detail/probe_table.cpp
	detail/screening.cpp

	circuit_breaker.cpp
	limits.cpp
	position.cpp
	rate_limiter.cpp
	working_ledger.cpp
)
add_library(exchange_engine::risk_management ALIAS risk_management)

target_include_directories(risk_management
	PUBLIC
	$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
	$<INSTALL_INTERFACE:include>
)

target_link_libraries(risk_management
	PUBLIC
	exchange_engine::trading_engine
	exchange_engine::market_data
)

generate_module_export_header(risk_management)
enable_coverage(risk_management)
set_warnings(risk_management)
enable_hardening(risk_management)
