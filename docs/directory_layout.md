# Directory Layout

The source tree and the responsibility of each module. Related subsystems are
grouped under a few top-level buckets (`core/`, `transport/`, `market-data/`,
`trading-engine/`, `app/`); within each, every directory is one subsystem with a
single responsibility, and dependencies point strictly downward so the graph
stays acyclic. `transport/` is a top-level peer of `market-data/` — a system
ingress used by every external input, not a market-data sub-concern. See
[architecture.md](architecture.md) for how the pieces interact at runtime.

## Overview

```text
exchange-engine/
├── benchmark/        Performance benchmarks
├── docs/             Architecture and design docs
├── test/             Unit and integration tests
└── src/
    ├── core/                Application infrastructure
    │   ├── concurrency/     Thread communication primitives
    │   ├── memory/          Allocation strategy
    │   ├── persistence/     Journaling, snapshots, replay
    │   ├── optimisation/    Performance helpers
    │   └── util/            Generic utilities
    ├── transport/           External I/O (WebSocket / REST / FIX / replay)
    ├── market-data/         Feed ingestion and normalization
    │   └── binance/
    ├── trading-engine/      Order handling and matching
    │   ├── event/           Command and event definitions
    │   ├── execution/       Command routing and execution
    │   ├── order_book/      Market state and matching
    │   └── strategy/        Order-generating algorithms
    └── app/                 Composition root: CLI, configuration, logging
```

Dependencies flow one way and never cycle:

```text
Transport ─┐
MarketData ─┴─► Event ─► Execution ─► OrderBook ─► Memory ─► Util
                          ├─► Concurrency ─► Util
                          └─► Persistence ─► Util
```

Forbidden edges keep policy out of low-level modules: `OrderBook → Transport`,
`OrderBook → Dispatcher`, `Memory → OrderBook`, `Persistence → MatchingEngine`,
`Concurrency → Execution`, `Transport → OrderBook`. New functionality goes in the
subsystem that owns the responsibility — avoid adding top-level modules.

## Modules

### core/ — application infrastructure

Startup, configuration, logging, service lifetime. **No trading logic.**

```text
core/
├── application.hpp
├── configuration.hpp
├── logger.hpp
├── service.hpp
├── clock.hpp
└── version.hpp
```

### transport/ — external I/O

Top-level system ingress (a peer of `market-data/`, not nested under it).
Sockets, protocol framing, connection lifecycle. Knows protocols, not order
books; decoding bytes into domain types is `market-data`'s job.

```text
transport/
├── rest.hpp/.cpp        REST fetch (e.g. depth snapshots)
├── websocket.hpp/.cpp   streaming diff-depth feed
├── replay.hpp/.cpp      JSONL capture replay
├── transport.hpp        aggregator (../transport.hpp)
└── (planned) fix/
```

### market-data/ — feed ingestion

Converts exchange-specific feeds into normalized events. **No matching logic.**
Flat under its bucket — no redundant `market_data/market_data/` nesting.

```text
market-data/
├── binance/          binance depth parsing (binance_depth, fwd)
├── parser.hpp
└── market_data.hpp   aggregator
```

### event/ — communication

Strongly typed commands and events exchanged between subsystems. Defines
communication, not execution.

```text
event/
├── command/
│   ├── place_order.hpp
│   ├── cancel_order.hpp
│   ├── replace_order.hpp
│   └── market_data_update.hpp
├── trade/
│   ├── trade_event.hpp
│   ├── execution_report.hpp
│   └── fill.hpp
├── market/
│   ├── depth_event.hpp
│   ├── quote_event.hpp
│   └── snapshot_event.hpp
├── lifecycle/
│   ├── startup.hpp
│   ├── shutdown.hpp
│   └── recovery.hpp
└── event.hpp
```

### execution/ — command routing and execution

Coordinates processing but owns little market state itself.

```text
execution/
├── dispatcher/        Route commands to a partition: hash(symbol) % partitions
├── engine_partition/  Owns queue, matching engine, book manager, buffers, sink
├── matching_engine/   Validate and execute commands; owns no market state
└── book_manager/      Create, own, look up, and remove OrderBooks by symbol
```

The **dispatcher** owns no state — routing only. The **matching engine** executes
commands and operates on a book it looks up through the **book manager**, which is
the sole owner of `OrderBook` instances.

### order_book/ — market state and matching

Bid/ask books, price-time priority, matching, cancellation. Single-threaded, no
synchronisation primitives.

```text
order_book/
├── order_book/
├── matcher/
├── level/
├── order_list/
└── order/
```

### concurrency/ — thread communication

Reusable lock-free primitives. Business logic never depends on their internals.

```text
concurrency/
├── queue/           SPSC / MPSC / MPMC
├── synchronisation/ hazard pointers, wait strategies
└── executor/
```

### memory/ — allocation

Isolates allocation strategy so the matching engine never allocates directly.

```text
memory/
├── object_pool/
├── allocator/
├── arena/
└── slab/
```

### persistence/ — durability

Journaling, snapshots, recovery, replay. Observes execution, never mutates state.

```text
persistence/
├── journal/
├── snapshot/
└── replay/
```

### strategy/ — algorithms

Generate commands submitted to the engine. Never touch an `OrderBook` directly.

```text
strategy/
├── iceberg/
├── twap/
├── vwap/
└── smart_order_router/
```

### optimization/ — performance helpers

Reusable performance code independent of business logic.

```text
optimization/
├── branchless/
├── simd/
├── cache/
├── prefetch/
└── compiler/
```

### util/ — generic utilities

Lightweight, dependency-free helpers (`expected`, `hash`, `timer`, `scope_exit`,
`string`).
