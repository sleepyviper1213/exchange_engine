# The engine, as five modules and a facade over them.
#
# This target used to be the whole matching engine in one library: the order
# vocabulary, the book, the event types, the execution layer and the reference
# data, all linked together because they happened to share a directory. The
# dependency order between them was real - orders <- order_book <- event <-
# execution - but it was maintained by convention and code review, and nothing in
# the build stopped the vocabulary reaching up into the partition.
#
# Now each layer is a target and that order is a link error. What is left here is
# a facade: `format.hpp` and the umbrella headers, which genuinely span all five
# (format.hpp writes formatters for orders, book records, commands, lifecycle
# records *and* the order manager), plus the link line that keeps every existing
# consumer working. Nothing outside src/trading-engine/ changed when this split
# happened - `exchange_engine::trading_engine` still means "the whole engine",
# and every `#include "trading-engine/..."` still resolves, because the include
# root is src/ and no directory moved.
#
# Depend on this when you mean the engine. Depend on one of the five when you
# mean that layer - `risk_management` and `strategy` want `event`, not a book.
add_subdirectory(orders)
add_subdirectory(order_book)
add_subdirectory(event)
add_subdirectory(execution)
add_subdirectory(symbol)

# INTERFACE because there is nothing left to compile: the facade is headers, and
# the five modules below carry every translation unit. An INTERFACE target rather
# than no target, for the reason strategy/ gives - the dependencies are declared
# where the build can check them instead of being inherited by accident.
add_library(trading_engine INTERFACE)
add_library(exchange_engine::trading_engine ALIAS trading_engine)

target_include_directories(trading_engine
	INTERFACE
	$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
	$<INSTALL_INTERFACE:include>
)

target_link_libraries(trading_engine
	INTERFACE
	exchange_engine::orders
	exchange_engine::order_book
	exchange_engine::event
	exchange_engine::execution
	exchange_engine::symbol
	fmt::fmt
	exchange_engine::core
	Boost::headers
)

# Carried over from the old library target: these silence warnings the *engine's
# own headers* provoke in consumers - MSVC's C4251/C4275 on exported types with
# standard-library members, and GCC's interference-size note. They belong on the
# facade because that is what consumers link.
target_compile_options(trading_engine INTERFACE
	$<$<CXX_COMPILER_ID:MSVC>:/wd4251 /wd4275>
	$<$<CXX_COMPILER_ID:GNU>:-Wno-interference-size>
)
