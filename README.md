# Exchange Engine

<div align="center">

**A high-frequency trading framework in modern C++23.**

![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)
![Platform](https://img.shields.io/badge/platform-Windows%20|%20Linux-lightgrey.svg)

</div>

---

## Overview

The engine models the full execution pipeline: receiving market data and client
commands, routing them, matching, and emitting trades.

Determinism comes from ownership rather than locking — each order book is owned
exclusively by a single engine thread, and threads communicate only through
lock-free message passing. This keeps locks off the matching path while
preserving correctness.

The goal of this project is **software architecture**, not exchange
completeness; each subsystem is isolated and independently testable.

## Features

**Matching** — price-time priority (FIFO per level), limit orders, partial
fills, multiple executions per incoming order, cancellation, batched trade
generation. Iceberg / stop / market orders are planned.

**Order book** — separate bid/ask books over sorted contiguous price levels,
pool-backed intrusive FIFO per level, O(1) cancel, O(log n) price lookup, no
heap allocation while matching once the node pool has warmed.

**Concurrency** — lock-free bounded SPSC queue, wait-free producer and consumer,
acquire/release ordering, cache-line-aligned control variables to avoid false
sharing.

**Market data** — Binance REST snapshots and WebSocket depth updates,
integer-scaled prices, L2 book reconstruction.

**Performance** — branchless binary search, cache-aware object pool, batch
command processing, zero-copy command transport.

## Performance targets

| Path | Target | Budget per item |
|------|--------|-----------------|
| Market data ingestion | 10M msg/s | 100 ns |
| Order book updates | 1M updates/s | 1 µs |
| Order processing | 100K orders/s | 10 µs |

Sustained steady-state on one machine. Ingestion is an aggregate across sharded
ingest cores — JSON decode alone exceeds the per-message budget on a single
core. See [docs/performance.md](docs/performance.md) for the budget derivation,
the current gaps against it, and how each target is measured.

## Architecture

```mermaid
flowchart LR
    subgraph Transport["Transport"]
        WS["WebSocket"]
        REST["REST"]
        CLIENT["Client API"]
    end
    PARSER["Parser"]
    ROUTER["Dispatcher"]
    subgraph Engine["Engine Partition"]
        Q["SPSC Queue"] --> ME["Matching Engine"] --> OB["Order Books"]
    end
    subgraph Output["Output"]
        TRADES["Trades"]
        JOURNAL["Persistence"]
        METRICS["Metrics"]
    end
    WS & REST & CLIENT --> PARSER --> ROUTER --> Q
    OB --> TRADES & JOURNAL & METRICS
```

Commands flow through fixed layers, each depending only on its neighbor. Only
the execution layer mutates market state. See
[docs/architecture.md](docs/architecture.md) for the execution model and
[docs/directory_layout.md](docs/directory_layout.md) for the source tree.

## Build

_TODO_

## Roadmap

- **Exchange** — engine partitioning, NUMA scheduling, symbol routing
- **Matching** — iceberg, stop, market, IOC/FOK
- **Infrastructure** — FIX gateway, persistence, replay, event sourcing
- **Performance** — intrusive order list, allocation-free ingest, ingest
  sharding, SIMD, huge pages, lock-free dispatcher
  ([targets](docs/performance.md))
- **Analytics** — latency profiler, metrics, benchmark suite
