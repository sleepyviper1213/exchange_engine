#pragma once
// Thin re-export: the book side enum lives in core/types.hpp as exchange::Side
// (shared with the wider engine). This header exists so order_book code can
// name the side type through a book-local path without redefining it.
// IWYU pragma: begin_exports
#include "core/types.hpp"
// IWYU pragma: end_exports
