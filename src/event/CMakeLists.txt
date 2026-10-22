add_library(event SHARED
	command.cpp
	engine_event.cpp
	journal_record.cpp
)
add_library(exchange_engine::event ALIAS event)

target_include_directories(event
	PUBLIC
	$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
	$<INSTALL_INTERFACE:include>
)

target_link_libraries(event
	PUBLIC
	exchange_engine::orders
	exchange_engine::core
)

target_compile_options(event PUBLIC
	$<$<CXX_COMPILER_ID:MSVC>:/wd4251 /wd4275>
)

generate_module_export_header(event)
enable_coverage(event)
set_warnings(event)
enable_hardening(event)
