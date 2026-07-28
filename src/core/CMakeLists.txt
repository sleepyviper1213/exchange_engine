add_library(
    core SHARED
    util/counted.cpp
	util/lowered.cpp
	util/round_up.cpp
	util/slurp.cpp
	concurrency/affinity/affinity.cpp
	concurrency/affinity/core_allocator.cpp
	concurrency/affinity/topology.cpp
    concurrency/synchronisation/hazard_pointer/hazard_pointer_domain.cpp
    concurrency/synchronisation/hazard_pointer/hazard_pointer_thread_cache.cpp
    concurrency/synchronisation/hazard_pointer/hazard_pointer_obj.cpp
	memory/arena.cpp
	memory/allocator.cpp
	memory/numa_arena_allocator.cpp
	memory/slab.cpp
)
add_library(exchange_engine::core ALIAS core)

target_include_directories(
    core
	PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
           $<INSTALL_INTERFACE:include>)

target_link_libraries(core PUBLIC fmt::fmt)

generate_module_export_header(core)
enable_coverage(core)
set_warnings(core)
enable_hardening(core)
