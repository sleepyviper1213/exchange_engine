# The vocabulary of things that happen: what the engine is asked, and what it
# reports having done.
#
# A module because this is the layer the rest of the system talks *through*.
# `risk_management` screens commands, `strategy` writes them, `session` logs them
# and `core::persistence` journals them - none of those needs a book or a
# matching engine, and until now linking to a command meant linking to both.
#
# It sits above `order_book` because an outcome names a trade and a trade is a
# book's product, and below `execution` because nothing here knows who applies a
# command. That ordering is the reason this module could not be extracted first:
# `event` needs `order_book`, and `execution` needs `event`, so pulling this out
# ahead of the book would have made a cycle between two targets.
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
	exchange_engine::order_book
	exchange_engine::orders
	exchange_engine::core
)

target_compile_options(event PUBLIC
	$<$<CXX_COMPILER_ID:MSVC>:/wd4251 /wd4275>
	$<$<CXX_COMPILER_ID:GNU>:-Wno-interference-size>
)

generate_module_export_header(event)
enable_coverage(event)
set_warnings(event)
enable_hardening(event)
