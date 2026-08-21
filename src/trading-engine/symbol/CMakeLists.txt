# Reference data: what a listing is, and whether an order is admissible against
# it before anything tries to match it.
#
# Off to the side of the engine's main chain rather than in it - it depends on
# the vocabulary and on `reject_reason`, and nothing in `order_book`, `event` or
# `execution` depends on it. Its own module for that reason: a listing's tick and
# lot sizes are a deployment's facts, and a fact needs no engine to be true.
add_library(symbol SHARED
	symbol_spec.cpp
	validation.cpp
)
add_library(exchange_engine::symbol ALIAS symbol)

target_include_directories(symbol
	PUBLIC
	$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
	$<INSTALL_INTERFACE:include>
)

target_link_libraries(symbol
	PUBLIC
	exchange_engine::order_book
	exchange_engine::orders
	exchange_engine::core
)

target_compile_options(symbol PUBLIC
	$<$<CXX_COMPILER_ID:MSVC>:/wd4251 /wd4275>
	$<$<CXX_COMPILER_ID:GNU>:-Wno-interference-size>
)

generate_module_export_header(symbol)
enable_coverage(symbol)
set_warnings(symbol)
enable_hardening(symbol)
