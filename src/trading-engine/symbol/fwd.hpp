#pragma once

#include "trading_engine_export.hpp"

#include <cstdint>

namespace exchange::engine {

/// @brief Dense identifier for a listing, assigned by the reference-data source.
///        Dense because it indexes the book manager's per-symbol arrays.
using symbol_id_t = std::uint32_t;

class symbol_spec;
struct OrderRequest;
struct SymbolRegistry;

} // namespace exchange::engine
