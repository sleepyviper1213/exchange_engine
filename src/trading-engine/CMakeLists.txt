add_library(trading_engine SHARED
    event/command.cpp
    event/engine_event.cpp
    execution/dispatcher.cpp
    execution/book_manager.cpp
    execution/book_snapshot.cpp
    execution/matching_engine.cpp
    execution/order_manager.cpp
    orders/order.cpp
    order_book/price_level.cpp
    order_book/order_book.cpp
    order_book/order_state.cpp
    order_book/outcome.cpp
    order_book/detail/resting_order.cpp
    order_book/detail/order_pool.cpp
    order_book/detail/book_side.cpp
    symbol/symbol_spec.cpp
    symbol/validation.cpp
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
	Boost::headers
)

target_compile_options(trading_engine PUBLIC
	$<$<CXX_COMPILER_ID:MSVC>:/wd4251 /wd4275>
	$<$<CXX_COMPILER_ID:GNU>:-Wno-interference-size>
)

generate_module_export_header(trading_engine)
enable_coverage(trading_engine)
set_warnings(trading_engine)
enable_hardening(trading_engine)
