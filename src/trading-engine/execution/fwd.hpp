#pragma once

#include "core/types.hpp"
#include "trading_engine_export.hpp"

#include <cstddef>

namespace exchange::engine::execution {

// The default argument lives here (declared once) so that including the
// definition header, which now omits it, still sees it.
//
// No dllexport/dllimport on this class template: it is header-only and
// instantiated per-TU, so there is no single exported symbol. Marking it would
// turn each consumer's MatchingEngine<N> members into __imp_ references the DLL
// never provides (LNK2019). Matches the object_pool/node_pool note in
// core/memory/fwd.hpp.
template <std::size_t QueueCapacity = 1U << 14>
class MatchingEngine;

// TODO: forward-declare the execution scaffold types (BookManager, Dispatcher,
// EnginePartition) once they gain real definitions.

} // namespace exchange::engine::execution