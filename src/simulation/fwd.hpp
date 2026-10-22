#pragma once

// Declarations for the simulated venue: the pieces that stand in for what a
// recording or a mirrored live feed cannot supply on its own - the depth a
// venue publishes turned into resting liquidity, a passive fill inferred from
// that depth, and the flight time of a command.
//
// A module of its own because two harnesses run it. `strategy/backtest` drives
// it from a capture and `session/` from a live socket, and production code must
// not reach into the backtest to get it. It links the market-data and engine
// vocabularies and nothing that executes either: no partition, no risk gate.

namespace exchange::simulation {

template <class T>
class delay_queue;

struct latency_model;
// `wire` is deliberately absent: its clock parameter is constrained, and a
// declaration that drops the constraint declares a different template rather
// than this one. Naming it means including wire.hpp anyway.

class queue_position_book;

struct fill_model_options;
template <class Sink>
class crossing_fill_model;

struct resting_quote;
class depth_feed_bridge;

} // namespace exchange::simulation
