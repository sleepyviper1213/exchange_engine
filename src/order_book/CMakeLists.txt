# The book: order by order, price first, for flow this process owns.
#
# A module because it is the layer everything above it matches against, and
# because the boundary is the one CLAUDE.md is most insistent about:
# `market_data::l2_book` reconstructs depth a *venue publishes* (aggregate per
# price) and this matches orders *this process originates* (order-by-order,
# FIFO). That split "is enforced by linkage, not convention" - and until now the
# linkage was the whole trading-engine target, which also contained the event and
# execution layers. Now it is this one.
#
# Links `orders` and `core` and nothing else. It knows what an order is and how
# to allocate; it does not know what a command is, what a partition is, or that
# anything is listening.
add_library(order_book SHARED
	price_level.cpp
	order_book.cpp
	order_state.cpp
	outcome.cpp

	detail/resting_order.cpp
	detail/order_pool.cpp
	detail/book_side.cpp
)
add_library(exchange_engine::order_book ALIAS order_book)

target_include_directories(order_book
	PUBLIC
	$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
	$<INSTALL_INTERFACE:include>
)

target_link_libraries(order_book
	PUBLIC
	exchange_engine::orders
	exchange_engine::core
	Boost::headers
)

target_compile_options(order_book PUBLIC
	$<$<CXX_COMPILER_ID:MSVC>:/wd4251 /wd4275>
	$<$<CXX_COMPILER_ID:GNU>:-Wno-interference-size>
)

generate_module_export_header(order_book)
enable_coverage(order_book)
set_warnings(order_book)
enable_hardening(order_book)
