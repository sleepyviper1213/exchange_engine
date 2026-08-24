#include "order_pool.hpp"

namespace exchange::engine::detail {

// The pool is a template so the price ladder can reuse it for its levels, but
// the order pool is the one every book has, so it is instantiated once here
// rather than in each translation unit that rests an order.
template class basic_pool<resting_order>;

} // namespace exchange::engine::detail
