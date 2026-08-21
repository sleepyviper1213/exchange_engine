# The vocabulary: what an order *is*, before anything does something with it.
#
# A module rather than a directory because it is the bottom of the engine's own
# graph and everything above it depends on it - order_book, event, execution and
# symbol all name these types, and none of them may be named back. That was true
# by convention and reviewed by hand; a target makes it a link error.
#
# It carries no dependency but `core/util/enum_string.hpp`, which is the honest
# measure of what vocabulary means here: no allocator, no clock, no book. If this
# target ever grows a link line, something has been put in the wrong place.
add_library(orders SHARED
	order.cpp
)
add_library(exchange_engine::orders ALIAS orders)

target_include_directories(orders
	PUBLIC
	$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
	$<INSTALL_INTERFACE:include>
)

target_link_libraries(orders
	PUBLIC
	exchange_engine::core
)

target_compile_options(orders PUBLIC
	$<$<CXX_COMPILER_ID:MSVC>:/wd4251 /wd4275>
	$<$<CXX_COMPILER_ID:GNU>:-Wno-interference-size>
)

generate_module_export_header(orders)
enable_coverage(orders)
set_warnings(orders)
enable_hardening(orders)
