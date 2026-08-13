#pragma once
// Umbrella header for the strategy module: the framework plus the strategies
// built on it. Prefer the narrowest header that compiles — a translation unit
// that only defines a strategy needs `command_writer.hpp` and nothing else.
// IWYU pragma: begin_exports
#include "strategy/command_writer.hpp"
#include "strategy/concepts.hpp"
#include "strategy/engine.hpp"
#include "strategy/iceberg.hpp"
#include "strategy/stop.hpp"
// IWYU pragma: end_exports
