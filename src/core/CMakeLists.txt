add_library(core SHARED
	util/slurp.cpp
	concurrency/synchronization/hazard_pointer/hazard_pointer_domain.cpp
	concurrency/synchronization/hazard_pointer/hazard_pointer_thread_cache.cpp
	concurrency/synchronization/hazard_pointer/hazard_pointer_obj.cpp
)
add_library(exchange_engine::core ALIAS core)

target_include_directories(core
	PUBLIC
	$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
)


include(GenerateExportHeader)
generate_export_header(core)

enable_coverage(core)
set_warnings(core)
enable_hardening(core)
