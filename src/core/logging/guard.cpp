#include "guard.hpp"

#include "lifecycle.hpp"
#include "settings.hpp"

namespace exchange::core::logging {

guard::guard(const settings &config) { init(config); }

// shutdown, not flush - and under settings::async that is not a preference.
//
// An async logger owns a worker thread, held by spdlog's registry, which is a
// static. Left alive, it is torn down during *static* destruction, and on
// Windows that runs while the loader lock is held: the join waits for the
// worker, the worker cannot finish exiting without the same lock, and the
// process hangs after main has already returned. Measured on MinGW, with the
// main thread parked in thread_pool::~thread_pool.
//
// So ending the scope has to end the logger, not merely flush it. That is what
// an RAII guard is for, and flush could not have carried it: an async flush is
// posted to the queue rather than performed. @see logging::shutdown
guard::~guard() { shutdown(); }

} // namespace exchange::core::logging
