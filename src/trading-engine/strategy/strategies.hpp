#pragma once
// Umbrella header for the strategy module: the framework plus the strategies
// built on it. Prefer the narrowest header that compiles — a translation unit
// that only defines a strategy needs `command_writer.hpp` and nothing else.
// IWYU pragma: begin_exports
#include "command_writer.hpp"
#include "concepts.hpp"
#include "engine.hpp"
#include "iceberg.hpp"
#include "stop.hpp"
// IWYU pragma: end_exports
