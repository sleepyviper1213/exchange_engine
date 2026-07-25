add_library(core SHARED
add_library(
    core SHARED
    util/slurp.cpp
    util/counted.cpp
    concurrency/synchronisation/hazard_pointer/hazard_pointer_domain.cpp
    concurrency/synchronisation/hazard_pointer/hazard_pointer_thread_cache.cpp
    concurrency/synchronisation/hazard_pointer/hazard_pointer_obj.cpp
	memory/arena.cpp)
add_library(exchange_engine::core ALIAS core)

target_include_directories(core
	PUBLIC
	$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
)


generate_module_export_header(core)

enable_coverage(core)
set_warnings(core)
enable_hardening(core)
