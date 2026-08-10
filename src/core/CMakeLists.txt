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
    memory/slab.cpp

    logging/lifecycle.cpp
    logging/guard.cpp
)
add_library(exchange_engine::core ALIAS core)

if(ORDER_BOOK_WITH_NUMA)
    target_sources(core PRIVATE memory/numa_arena_allocator.cpp)
endif()

target_include_directories(
    core
    PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>)

# Boost.Pool is PUBLIC because it is in a header, not just a source: object_pool
# is a template, so every target that pools an object compiles boost::pool too.
target_link_libraries(
    core
    PUBLIC
    fmt::fmt
    spdlog::spdlog
    exchange_engine::numa
    Boost::headers)


target_compile_options(
    core
    PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/wd4251 /wd4275 /wd4324>
    $<$<CXX_COMPILER_ID:GNU>:-Wno-interference-size>
)

# Compile-time log floor. This is what makes logging safe to reach for on a hot
# path: spdlog::trace/spdlog::debug below this level expand to nothing at all — no
# call, no argument evaluation, no format string left in the binary — so a
# spdlog::debug inside l2_book::set_level costs exactly zero in a Release build
# while still being available under a Debug one. The *functions* spdlog::debug()
# et al. ignore this and always pay a runtime check, which is why core/logging.hpp
# tells hot-path callers to use the macros.
#
# PUBLIC because it has to agree across every target: spdlog headers are shared,
# and a translation unit compiled with a different floor would disagree with the
# rest about which calls exist.
target_compile_definitions(core PUBLIC
    $<IF:$<CONFIG:Debug>,SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE,SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO>)

generate_module_export_header(core)
enable_coverage(core)
set_warnings(core)
enable_hardening(core)
