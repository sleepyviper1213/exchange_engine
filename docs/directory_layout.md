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
    ├── market-data/         Feed ingestion and normalisation
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
Transport ──► Core
MarketData ─► Core                    (no edge to the trading engine)
Event ─► Execution ─► OrderBook ─► Memory ─► Util
          ├─► Concurrency ─► Util
          └─► Persistence ─► Util
App ────► Transport + MarketData + TradingEngine
```

`market-data` and `trading-engine` are **siblings, not a chain**: neither links
the other. Market data decodes a venue's feed into its own `l2_book`; the engine
matches orders this process originated. Only the composition root (`app/`, and
the benchmarks) links both, and it is the one place that joins them.

Forbidden edges keep policy out of low-level modules: `OrderBook → Transport`,
`OrderBook → Dispatcher`, `Memory → OrderBook`, `Persistence → MatchingEngine`,
`Concurrency → Execution`, `Transport → OrderBook`. New functionality goes in the
subsystem that owns the responsibility — avoid adding top-level modules.

## Cross-cutting concerns

A facility that applies to every module — formatting, serialisation, hashing,
journal encoding, test matchers — is an **opt-in sidecar header inside each
module**, never a module of its own. `market-data/format.hpp` and
`trading-engine/format.hpp` are the existing pair; a future
`<module>/serialise.hpp` would take the same shape.

The reason is the dependency graph, not taste. A central `format/` module would
have to include every type in the system in order to see them, pointing an edge
from the bottom of the graph back up to the top and turning a rule the build can
enforce into one only review can. A sidecar keeps every edge pointing down: it
depends on its own module and on the third-party library, and nothing depends on
it except the translation unit that opted in.

Three properties follow, and all three are the point:

- **Excluded from the aggregator.** `market_data.hpp` pulls in the domain
  headers and no formatter, so a translation unit that never prints a book never
  pays for `<fmt/format.h>`.
- **Forgetting is a compile error.** The alternative — the domain header
  defining the formatter — fails silently instead, rendering one way in a
  translation unit that saw it and another in one that did not.
- **The module keeps ownership.** Types and the code that renders them are
  reviewed together and move together; relocating a subsystem never strands a
  formatter in a central header that has to be edited to follow.

fmt lays out its own `fmt/std.h` and `fmt/ranges.h` this way for the same
reason, and Abseil arrives from the other direction with `AbslStringify`: one
ADL hook each type opts into, rather than a formatting library that knows every
type.

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

Venue-agnostic, and that includes *addresses*: the diff-depth stream's host,
port and `/ws/<symbol>@depth@100ms` grammar are Binance facts, so they live in
`market-data/binance/endpoints.hpp`. Callers resolve an endpoint there and hand
the resulting `{host, port, target}` to `ws::capture` — transport never spells a
stream name.

```text
transport/
├── rest.hpp/.cpp        REST fetch (e.g. depth snapshots)
├── websocket.hpp/.cpp   streaming diff-depth feed
├── replay.hpp/.cpp      JSONL capture replay
├── transport.hpp        aggregator (../transport.hpp)
└── (planned) fix/
```

### market-data/ — feed ingestion

Converts exchange-specific feeds into normalised events. **No matching logic.**
Flat under its bucket — no redundant `market_data/market_data/` nesting.

```text
market-data/
├── binance/          binance depth parsing (binance_depth, endpoints, fwd)
├── parser.hpp
├── l2_book.hpp/.cpp  local L2 book: reconstructs the venue's published depth
├── format.hpp        fmt formatters (opt-in)
├── fwd.hpp
└── market_data.hpp   aggregator
```

`l2_book` and `order_book` are two different concepts and live in two different
subsystems on purpose. `market_data::l2_book` is a **local order book**: price
levels with one aggregated quantity each, rebuilt from the venue's published
depth (a REST snapshot seeded, then `<symbol>@depth` diffs applied — the
managed-local-order-book procedure Binance documents). `engine::order_book` is
an **order-by-order matching engine**: FIFO queues of individual orders with
price-time priority, holding state this process owns rather than state a venue
publishes. A diff-depth feed can only ever produce the former.

The dividing line is **provenance, not depth level**. It is not that market data
is inherently L2 — given an order-by-order feed (Nasdaq ITCH, Coinbase's full
channel) it would reconstruct a genuine L3 book, still in `market-data/`, still
not in the matching engine. The rule is: state the venue publishes goes in one,
state this process originates goes in the other.

That is enforced structurally rather than by convention. Every apply entry point
—`apply_depth_update`, `apply_binance_depth_update`, `DepthParser::apply_update`
— takes `l2_book&`, and `market_data` does not link `trading_engine`, so a
decoder *cannot* be handed an `order_book`. Pointing one at the wrong book used
to compile: `order_book::set_level` collapses a level to a single anonymous
entry, which reconstructs correctly but leaves a book whose synthetic orders
carry no id (so `cancel_order` cannot see them) and whose FIFO order is
invented. It also costs about 5× the memory per level (16 B flat vs ~80 B plus a
heap block) and measurably more time — see `BM_MarketReplay_SteadyState` against
`BM_MarketReplay_L2Book`, which keeps that comparison alive using a fixture
local to the benchmark.

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
synchronisation primitives. Order-by-order (L3) only — the aggregate-by-price
reconstruction book is `market-data/l2_book`.

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

### optimisation/ — performance helpers

Reusable performance code independent of business logic.

```text
optimisation/
├── branchless/
├── simd/
├── cache/
├── prefetch/
└── compiler/
```

### util/ — generic utilities

Lightweight, dependency-free helpers (`expected`, `hash`, `timer`, `scope_exit`,
`string`).

## Making a type printable

Two mechanisms, and which one to reach for is decided by the type, not by taste
([fmt docs](https://fmt.dev/12.0/api/#formatting-user-defined-types)). Providing
both for one type is disallowed.

**`format_as`** — for enums and anything that already has a string stand-in. An
ADL free function returning an already-formattable type. **You do not write it
for an enum**: the `EXCHANGE_ENUM_*` X-macros in `core/util/enum_string.hpp`
emit it alongside the accessor, so one declaration gives you both.

```cpp
enum class parse_error : std::uint8_t { EXCHANGE_ENUM_VALUES(PARSE_ERROR_LIST) };
EXCHANGE_ENUM_LABEL(parse_error, message, PARSE_ERROR_LIST)
// -> message(e)                     "empty number"   (string_view, constexpr)
// -> fmt::format("{}", e)           "empty number"
// -> fmt::to_string(e)              "empty number"   (owned std::string)
```

That is the project's **uniform enum-to-string conversion**. There is no
per-enum `to_string` returning `std::string`: `fmt::to_string` is the one
spelling that allocates, and a caller wanting a view keeps the generated
accessor and allocates nothing. It costs the enum's header no fmt include, and
the enum inherits the string format specifiers, so `{:>8}` works.

Use `EXCHANGE_ENUM_NAME` when the text is the enumerator's own identifier
(`Side`, `OrderType`) and `EXCHANGE_ENUM_LABEL` when it is a message the name
cannot carry (`depth_error`, `depth_speed`, `parse_error`). An enum needing
*both* accessors uses the `_ONLY` variants for one of them and
`EXCHANGE_ENUM_FORMAT_AS` to pick the display form — two hooks for one type is a
redefinition.

Generic code that means "any of our enums" constrains on
`core::util::FormattableEnum` rather than `std::is_enum_v`, so an enum without
the hook fails at the call site instead of inside fmt's argument machinery.

**`fmt::formatter<T>`** — for composite records with no such stand-in. These
need `<fmt/format.h>`, so they go in a module's opt-in `format.hpp`
(`market-data/format.hpp`, `trading-engine/format.hpp`) rather than the domain
header — the sidecar shape from [Cross-cutting concerns](#cross-cutting-concerns),
and the one fmt uses for its own `fmt/std.h` and `fmt/ranges.h`. Nothing includes
them implicitly; a translation unit that formats one of these types includes the
header itself, and forgetting is a compile error rather than a different
rendering. Derive from `fmt::nested_formatter<std::string_view>` so fill, align
and width apply to the whole record — `{:>32}` right-aligns a `Trade` in a log
column.

Where a type needs both a formatter and an owned-string accessor, the accessor
is defined in terms of the formatter (`binance::message` is
`fmt::format("{}", error)`), never rendered twice.
