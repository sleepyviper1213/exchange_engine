# What applies a command: the matching engine, the partition that owns a thread's
# books, and the record store behind them.
#
# The top of the engine's own graph, and the last piece to leave the old
# trading_engine target. It links `event` because it consumes commands and
# publishes engine events, `order_book` because a partition owns books, and
# `core` for the SPSC queue, the metrics and - the one worth naming -
# `core::persistence`, since `engine_partition` journals every command it applies
# before applying it.
#
# Note what is *not* here: no market data, no risk, no strategy. A partition is
# handed commands and reports what happened; who screened them and who wrote them
# is somebody else's layer. That was true before and is now a link line.
add_library(execution SHARED
	dispatcher.cpp
	book_manager.cpp
	book_snapshot.cpp
	matching_engine.cpp
	order_manager.cpp
)
add_library(exchange_engine::execution ALIAS execution)

target_include_directories(execution
	PUBLIC
	$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
	$<INSTALL_INTERFACE:include>
)

target_link_libraries(execution
	PUBLIC
	exchange_engine::event
	exchange_engine::order_book
)

target_compile_options(execution PUBLIC
	$<$<CXX_COMPILER_ID:MSVC>:/wd4251 /wd4275>
	$<$<CXX_COMPILER_ID:GNU>:-Wno-interference-size>
)

generate_module_export_header(execution)
enable_coverage(execution)
set_warnings(execution)
enable_hardening(execution)
