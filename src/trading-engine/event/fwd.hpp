#pragma once

#include "trading_engine_export.hpp"

namespace exchange::engine::event {
struct LevelChange;
// Not class-level exported: Command exports its factory functions per-member
// (see command.hpp), so marking the whole struct dll-interface would make MSVC
// reject those member exports (C2487).
struct Command;
} // namespace exchange::engine::event