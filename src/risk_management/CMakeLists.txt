add_library(risk_management SHARED
	hooks/post_trade/fill_burst.cpp
	hooks/post_trade/monitor.cpp
	hooks/post_trade/order_trade_ratio.cpp
	hooks/post_trade/outcome_silence.cpp

	hooks/pre_trade/detail/position_entry.cpp
	hooks/pre_trade/detail/probe_table.cpp

	hooks/pre_trade/duplicate.cpp
	hooks/pre_trade/position.cpp
	hooks/pre_trade/price_collar.cpp
	hooks/pre_trade/rate_limiter.cpp
	hooks/pre_trade/working_ledger.cpp

	hooks/system/circuit_breaker.cpp
	hooks/system/heartbeat.cpp
	hooks/system/pnl_drawdown_breaker.cpp

	limits.cpp
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
