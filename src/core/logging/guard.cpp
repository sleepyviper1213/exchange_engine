#include "guard.hpp"

#include "lifecycle.hpp"

#include "settings.hpp"

namespace exchange::core::logging {

guard::guard(const settings &config) { init(config); }

// flush, not shutdown: the two are equivalent for getting messages onto disk,
// and shutdown additionally leaves the default logger null, so anything logging
// later dereferences it. Flushing is the same benefit without that edge.
guard::~guard() { flush(); }

} // namespace exchange::core::logging
