#pragma once
// Every formatter the engine offers, in one include.
//
// The formatters themselves live with the types they print - one opt-in sidecar
// per module, which is what CLAUDE.md asks for: a cross-cutting facility belongs
// *inside* a module, never in a central one, because a central one points an edge
// back up the graph. This was that central one while the engine was a single
// library; now it is a facade over five, exactly like the target it belongs to.
//
// Include this when a translation unit prints records from more than one layer -
// a log line with a command and its resulting trade, say. Include the narrower
// one when it does not: `orders/format.hpp` alone is a fraction of the parse.

// IWYU pragma: begin_exports
#include "event/format.hpp"
#include "execution/format.hpp"
#include "order_book/format.hpp"
#include "orders/format.hpp"
// IWYU pragma: end_exports
