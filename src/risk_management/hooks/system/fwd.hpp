#pragma once
// Declarations for the out-of-band hooks - the ones that stop everything rather
// than refusing one order.
//
// `circuit_breaker` is the state all three of them write and every pre-trade
// rule reads, so it lives here with them: a drawdown, a breach-rate cut-out and
// a dead feed are three ways of arriving at the same switch. @see
// global_kill_switch.hpp

#include <cstdint>

namespace exchange::risk::hooks::system {

enum class trading_state : std::uint8_t;
enum class trip_cause : std::uint8_t;

class circuit_breaker;
class heartbeat_monitor;

} // namespace exchange::risk::hooks::system
