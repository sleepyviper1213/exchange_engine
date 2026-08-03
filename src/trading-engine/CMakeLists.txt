add_library(trading_engine SHARED
    order_book/level.cpp
    order_book/order.cpp
    order_book/order_book.cpp
    order_book/order_state.cpp
    order_book/outcome.cpp
    order_book/detail/resting_order.cpp
    order_book/detail/book_side.cpp
    order_book/detail/order_list.cpp
    symbol/symbol_spec.cpp
    symbol/validation.cpp
    event/command.cpp
)
add_library(exchange_engine::trading_engine ALIAS trading_engine)

target_include_directories(trading_engine
	PUBLIC
	$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
	$<INSTALL_INTERFACE:include>
)

target_link_libraries(trading_engine
	PUBLIC
	fmt::fmt
	exchange_engine::core
)

target_compile_options(trading_engine PUBLIC
	$<$<CXX_COMPILER_ID:MSVC>:/wd4251>
	$<$<CXX_COMPILER_ID:MSVC>:/wd4275>
)

generate_module_export_header(trading_engine)
enable_coverage(trading_engine)
set_warnings(trading_engine)
enable_hardening(trading_engine)
