# The pre-trade gate. Almost all of it is inline by design — see
# circuit_breaker.cpp for which parts are not, and why the rest may not be — but
# it is a module with a target like any other, so it gets the export header, the
# warning set and the hardening the rest of the tree runs under.
add_library(risk_management SHARED
    circuit_breaker.cpp
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
	exchange_engine::core
)

generate_module_export_header(risk_management)
enable_coverage(risk_management)
set_warnings(risk_management)
enable_hardening(risk_management)
