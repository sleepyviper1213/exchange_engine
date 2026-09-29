# Directory Layout

The source tree and the responsibility of each module. Related subsystems are
grouped under a few top-level buckets (`core/`, `transport/`, `market_data/`,
``, `app/`); within each, every directory is one subsystem with a
single responsibility, and dependencies point strictly downward so the graph
stays acyclic. `transport/` is a top-level peer of `market_data/` — a system
ingress used by every external input, not a market_data sub-concern. See
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
    │   ├── simd/            Vector kernels over a price ladder
    │   ├── chrono/          Clocks: monotonic, wall, ingress, replay
    │   └── util/            Generic utilities
    ├── transport/           External I/O (WebSocket / REST / FIX / replay)
    ├── venue/               What a venue's wire protocol says, both directions
    │   └── binance/         hosts, error envelope, exchangeInfo grid
    ├── market_data/         Feed ingestion and normalisation
    │   └── binance/
    ├──       Order handling and matching
    │   ├── event/           Command and event definitions
    │   ├── execution/       Command routing and execution
    │   ├── order_book/      Market state and matching
    │   ├── orders/          The order vocabulary
    │   └── symbol/          Listing reference data and validation
    ├── risk_management/     Pre-trade, post-trade and system risk hooks
    ├── strategy/            Order-generating algorithms
    │   └── backtest/        Offline harness: recorded venue, real engine
    └── app/                 Composition root: CLI, configuration, logging
        └── commands/        One driver per subcommand; cli.cpp only wires options
```

Dependencies flow one way and never cycle:

```text
Transport ──► Core
Venue ─────► Core                     (no edge to transport — see below)
MarketData ─► Core + Venue            (no edge to the trading engine)
Event ─► Execution ─► OrderBook ─► Memory ─► Util
          ├─► Concurrency ─► Util
          └─► Persistence ─► Util
Risk ─────► TradingEngine
Strategy ─► TradingEngine + Risk   (backtest/ additionally ─► MarketData)
App ──────► Transport + Venue + MarketData + TradingEngine + Risk + Strategy
```

`market_data` and `trading-engine` are **siblings, not a chain**: neither links
the other. Market data decodes a venue's feed into its own `l2_book`; the engine
matches orders this process originated. Joining them is therefore something only
a module above both may do, and exactly one thing does:
`strategy/backtest/depth_feed_bridge.hpp`, which turns a venue's published depth
into resting liquidity the matching engine can trade against.

Forbidden edges keep policy out of low-level modules: `OrderBook → Transport`,
`OrderBook → Dispatcher`, `Memory → OrderBook`, `Persistence → MatchingEngine`,
`Concurrency → Execution`, `Transport → OrderBook`. New functionality goes in the
subsystem that owns the responsibility — avoid adding top-level modules.

`strategy/` and `risk_management/` are a third pair that never link each other, and the
mechanism is worth naming because it generalises: they meet through a
*structural* concept (`command_sink`) rather than a shared header, so a risk gate
can be spliced into the command path without either side acquiring a dependency.
When two modules must compose but neither should own the other, that is the tool
to reach for.

## Cross-cutting concerns

A facility that applies to every module — formatting, serialisation, hashing,
journal encoding, test matchers — is an **opt-in sidecar header inside each
module**, never a module of its own. `market_data/format.hpp` and
`format.hpp` are the existing pair; a future
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
├── chrono/                 monotonic, wall, ingress and replay clocks
└── version.hpp
```

### transport/ — external I/O

Top-level system ingress (a peer of `market_data/`, not nested under it).
Sockets, protocol framing, connection lifecycle. Knows protocols, not order
books; decoding bytes into domain types is `market_data`'s job.

Venue-agnostic, and that includes *addresses*: the diff-depth stream's host,
port and `/ws/<symbol>@depth@100ms` grammar are Binance facts, so they live in
`market_data/binance/endpoints.hpp`. Callers resolve an endpoint there and hand
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

### venue/ — what a venue's wire protocol says

Everything about a venue that **both directions of traffic need**, and nothing
that moves bytes. Market data reads depth from a venue; an order gateway writes
orders to it; the two meet in more places than is obvious, and each of those is
a thing that must not exist twice:

```text
venue/
├── endpoint.hpp      {host, port, target} — what transport is handed
├── environment.hpp   production vs testnet, one switch for the whole process
├── weight_budget.hpp/.cpp  the venue's rate-limit allowance, counted once
├── binance/
│   ├── host.hpp          the four hostnames, per environment
│   ├── api_error.hpp/.cpp    the {code,msg} envelope every refusal carries
│   └── exchange_info.hpp/.cpp  the tick/lot grid, and the endpoint for it
├── format.hpp        fmt formatters for the endpoint types (opt-in)
├── fwd.hpp
└── venue.hpp         aggregator
```

**Why a module and not a corner of `market_data/binance/`.** Three of these are
shared *state or truth*, not shared convenience. The rate-limit allowance is the
sharpest: Binance limits by IP address, so a depth poller and an order gateway
spend from one budget — two components each tracking their own half would each
stay under the limit and together earn a 418 ban. The environment switch is the
same shape: a run reading production depth while trading on testnet reacts to a
book it is not in. And the error envelope is identical on a refused snapshot and
a refused order, so one decoder means one place to fix a wire change.

**Why it has no `transport` edge.** `venue/` hands out a resolved endpoint and a
description of a request; something else opens the socket. That is the same
contract `market_data/binance/endpoints.hpp` already had, kept deliberately:
were `venue` to link `transport`, then `market_data → venue → transport` would
give market data the transport dependency it has always been written not to
have. The bytes are moved one level up, in `session/`.

This is the **Adapter** side of the venue seam — vocabulary translation, in both
directions — and it is deliberately not a Proxy. A proxy preserves the interface
of what it stands for; the thing on the other side of this seam has failure
modes an `EXCHANGE` does not (rate limits, partitions, an ack that arrives
after a fill), and hiding that behind a local-looking interface is how a caller
comes to treat a timeout as a rejection and double-sends.

### market_data/ — feed ingestion

Converts exchange-specific feeds into normalised events. **No matching logic.**
Flat under its bucket — no redundant `market_data/market_data/` nesting.

```text
market_data/
├── binance/          binance depth and trade parsing (binance_depth,
│                     binance_trade, endpoints, fwd); hosts, error envelope
│                     and the trading grid are `venue/`
│   ├── depth_feed.hpp/.cpp  the venue's side of the feed seam: a JSONL capture
│   └── trade_feed.hpp/.cpp  the same for the tape
├── parser.hpp
├── l2_book.hpp/.cpp  local L2 book: reconstructs the venue's published depth
├── normalised.hpp    the venue-neutral event/snapshot every decoder produces
├── sequencer.hpp     gap detection over a diff feed's sequence numbers
├── reconstructor.hpp book + sequencer + pending buffer: the managed procedure
├── feed.hpp          the feed seam: `depth_feed`, `feed_handler`, `drive`
├── trade_feed.hpp    the tape seam: `trade_feed`, `trade_handler`, `drive`
├── format.hpp        fmt formatters (opt-in)
├── fwd.hpp
└── market_data.hpp   aggregator
```

**`feed.hpp` is where a second venue plugs in.** `normalised.hpp` already said
what a decoder must produce; what was missing was the shape of the thing
producing it, so every driver in the tree stopped one step short of
venue-neutral and named Binance in the middle of an otherwise generic loop:

```text
slurp(path) → binance::parse_binance_depth_updates → binance::normalise
            → depth_reconstructor
```

A `depth_feed` is the first three collapsed behind `next()`, and `drive(feed,
handler)` is the loop. Two properties are load-bearing:

- **`depth_reconstructor` satisfies `feed_handler` as written.** The handler a
  driver targets is the component that already existed, not an interface added
  for the driver's benefit.
- **Snapshots and diffs are one ordered stream**, not two sources. The order
  between them *is* the managed-local-order-book procedure — a snapshot arriving
  after events it should have preceded is a resync — and only the reconstructor
  can tell the difference, which it can only do if it sees them in arrival
  order.

It is deliberately not the steady-state hot path: a feed materialises each
frame's levels into a `depth_event`, which is what the reconstructor needs
(an event may be retained across a snapshot fetch) and what a consumer that only
ever applies in sequence should avoid — that one still wants
`depth_parser::apply_update` with `sequence_of`, which writes straight into the
book and builds no event at all.

**The tape is a second seam, not a third message kind.** A venue publishes two
things about a market — what is *resting* (`@depth`) and what actually *traded*
(`@trade`) — and `trade_feed.hpp` is the second of them: `trade_print`,
`trade_feed`, `trade_handler`, and a `drive` overload. The obvious alternative
was to widen `feed_message` to a third variant alternative, and it is wrong
twice over.

- **The ordering argument does not extend to a print.** A diff and a snapshot
  share a stream because the order between them is the reconstruction
  procedure. A print has no such relationship with either: it does not seed the
  book, does not repair it, and applying one to an `l2_book` is not an operation
  that means anything. A shared variant would assert a sequencing relationship
  that does not exist.
- **It would break every existing consumer.** `feed_handler` requires
  `on_event` and `on_snapshot`; a third alternative makes every handler in the
  tree incomplete — `depth_reconstructor` included — and `drive` would have to
  decide what to do with a message its handler cannot take. The cost lands
  entirely on the depth path, which gained nothing.

What *is* shared is `feed_status` and `feed_stop`, because those really are
message-neutral: "end of feed" and "malformed frame" mean the same thing on a
tape as on a book. `drive` is overloaded rather than renamed, and the two
overloads can never be ambiguous — no type satisfies both `depth_feed` and
`trade_feed`, since `next()` would have to return two different types.

A tape has no snapshot and no resync. Its only continuity signal is the venue's
trade id, which increments by one per print on one symbol, so a consumer detects
a dropped print by arithmetic on consecutive ids — and, unlike a depth gap,
cannot repair it, because there is nothing to reseed from. `exchange_tool trades`
reports that count for a recording, alongside the arrival statistics a tape is
actually kept for.

**Live ingest is a pipeline, and cannot be a `depth_feed`.** `next()` is
synchronous, so a socket could only satisfy it by blocking — and blocking is
exactly what a live feed must not do, because the snapshot that repairs a dead
replica has to be fetched *while frames keep arriving*. The frames that land
during the fetch are the ones that bridge the snapshot to the present; stop
reading to fetch and the gap grows wider than the snapshot can close.

So the live side replaces `drive()` rather than implementing `depth_feed`, and it
lives in [src/session/live_feed.hpp](../src/session/live_feed.hpp) — `market_data` links
neither Boost nor `transport`, and `transport` must not know what a
`depthUpdate` is, so nothing below can host the join. It sits with its only
caller, the way `depth_feed_bridge` does.

```text
ws frames ──▶ decode ──▶ on_event ──▶ depth_reconstructor ──▶ l2_book
                                          ▲
REST fetch ──▶ parse ──▶ channel ─────────┘   (drained by the frame loop)
```

Two chains of asynchronous work overlap in time, and **only one of them writes.**
The first version had both calling into the reconstructor, justified by
scheduling — one `io_context`, one thread, a coroutine yields only at a
`co_await`, so they cannot interleave mid-mutation. That is true, and it is the
wrong kind of true: it makes the design correct because of *how it is run*
rather than *what it is*, and it fails silently the day someone runs the context
on two threads.

The fetch chain now never touches the replica. It posts its outcome — depth or
failure — into an `asio::experimental::channel`, and the frame loop drains that
channel with a non-blocking `try_receive` once per frame. **Ownership rather than
synchronisation**, which is the rule this codebase states first and the same one
the matching engine's books rest on. Three things fall out:

- the fetch captures a `shared_ptr` to the channel and *nothing else* — no
  handler, no pointer into the coroutine frame — so it cannot dangle however the
  pipeline exits, and the join that used to be a `while (in_flight) sleep(10ms)`
  poll is gone;
- "is a fetch outstanding" became frame-loop-local state, so there is nothing
  left to race over;
- draining once per frame is free: frames arrive every 100 ms, a REST snapshot
  takes considerably longer than one to fetch.

Draining with `try_receive` rather than racing the channel against the socket
read is deliberate — `||` would cancel the loser, and cancelling a WebSocket read
mid-frame is not something a stream recovers from.

The pieces this needed:

- `transport::ws::stream_reader` — a pull-based frame reader (`connect` / `read`
  / `close`), the asynchronous mirror of `depth_feed::next()`. `capture_to_file`
  is now written on top of it, so the resolve/connect/TLS/upgrade sequence exists
  once.
- `binance::depth_frame_decoder` — the frame→event step on its own, shared by
  `jsonl_depth_feed` and the live pipeline, because only the *route* the bytes
  arrive by differs.
- `app::live_handler` — `feed_handler` plus the four snapshot members
  (`needs_snapshot`, `snapshot_requested`, `snapshot_failed`, `invalidate`).
  `depth_reconstructor` satisfies it as written.

`l2_book` and `EXCHANGE` are two different concepts and live in two different
subsystems on purpose. `market_data::l2_book` is a **local order book**: price
levels with one aggregated quantity each, rebuilt from the venue's published
depth (a REST snapshot seeded, then `<symbol>@depth` diffs applied — the
managed-local-order-book procedure Binance documents). `engine::EXCHANGE` is
an **order-by-order matching engine**: queues of individual orders at each
price, allocated by the policy the book was built with, holding state this
process owns rather than state a venue publishes. A diff-depth feed can only ever produce the former.

The dividing line is **provenance, not depth level**. It is not that market data
is inherently L2 — given an order-by-order feed (Nasdaq ITCH, Coinbase's full
channel) it would reconstruct a genuine L3 book, still in `market_data/`, still
not in the matching engine. The rule is: state the venue publishes goes in one,
state this process originates goes in the other.

That is enforced structurally rather than by convention. Every apply entry point
—`apply_depth_update`, `apply_binance_depth_update`, `depth_parser::apply_update`
— takes `l2_book&`, and `market_data` does not link `trading_engine`, so a
decoder *cannot* be handed an `EXCHANGE`. Pointing one at the wrong book used
to compile: `EXCHANGE::set_level` collapses a level to a single anonymous
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

Bid/ask books, price priority across levels and a configurable allocation
policy within one (price-time or pro-rata), matching, cancellation, queue
position. Single-threaded, no synchronisation primitives. Order-by-order (L3)
only — the aggregate-by-price reconstruction book is `market_data/l2_book`.

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

Generate commands submitted to the engine. Never touch an `EXCHANGE` directly:
a strategy is handed what the engine published and writes commands into a buffer
somebody else owns.

```text
strategy/
├── concepts.hpp        what a strategy is, as compile-time capability tests
├── command_writer.hpp  the bounded output cursor
├── command_batch.hpp   the inline storage the cursor writes into
├── engine.hpp          strategy_engine — the host that fans events out
├── iceberg.hpp
├── stop.hpp
├── quoter.hpp          the reference trader — opt-in, pulls market_data
└── backtest/           the offline harness — see below
```

`strategy_engine` composes its strategies by value in a tuple, so each hook is a
direct call and a stream nobody subscribed to compiles away entirely rather than
becoming a loop that runs zero times. `clocked` is declared with no implementor
yet — TWAP and VWAP are the obvious ones and are not built.

**Mostly, but no longer entirely, header-only.** The strategies themselves have
to stay in headers: every one of them is either a template or composed by value
into that tuple, and both instantiate in the consumer's translation unit. What
moved into `strategy` as a real shared library is the handful of types that are
*not* templates and were being recompiled into every consumer for nothing —
`command_writer`, the bridge, and the queue-position book:

| Compiled | Why it can be |
| --- | --- |
| `command_writer.cpp` | the cursor is deliberately not a template, so a strategy takes `command_writer &` rather than becoming a template on somebody else's buffer size |
| `backtest/depth_feed_bridge.cpp` | one concrete class over one listing; nothing about it varies per caller |
| `backtest/queue_position.cpp` | likewise — a book of rows, not a policy |

`command_batch<N>` stays a template and stays in a header, which is the split
`command_writer.hpp` has always described: the batch owns storage sized at
compile time, the cursor over it does not know how big it is.

The cost of the move is that every symbol crossing the library boundary needs
`STRATEGY_EXPORT`, applied per member as the rest of the tree does it. The
benefit is that `exchange_test`, `exchange_bench` and `exchange_tool` stop
compiling the same non-template bodies three times over.

#### strategy/backtest/ — the offline harness

Runs a recorded venue against this process's own matching engine, on one thread,
with no network and no wall clock. Full design in
[backtesting.md](backtesting.md).

```text
strategy/backtest/
├── depth_feed_bridge.hpp  the market_data/trading-engine join
├── fill_model.hpp  the one judgement the recording cannot make on its own
├── queue_position.hpp  how much of the venue's queue was ahead of ours
├── scheduler.hpp   delay_queue — a total order over what is in flight
├── wire.hpp        the modelled flight time, between the gate and the model
├── report.hpp      what a run measured
├── format.hpp      fmt formatters (opt-in sidecar)
└── session.hpp     the harness: bridge + gate + wire + partition + fill model
```

`scheduler.hpp` is the only one of these with no domain in it at all — a
`delay_queue<T>` over trivially copyable payloads, whose entire content is the
`(due_time, sequence)` total order. It is separate from `wire.hpp` because the
ordering guarantee is worth stating and testing without reference to commands,
latency, or anything a venue does; `wire.hpp` is the one that knows a command
travels on a wire, and it sits between the gate and the fill model because that
is where the network sits in a deployment.

`spread_quoter` used to live here and now sits in `strategy/quoter.hpp`, one
level up: `serve` drives the same quoter a backtest does, and a component two
callers share should not sit inside one of them. Like `strategy/backtest.hpp` it
is kept out of `strategy.hpp`, because its market hook takes an `l2_book` and
every other strategy here needs no decoder at all.

It belongs to `strategy/` because it exists to answer a question about a
strategy, and because it is the one consumer of the strategy hooks that is
allowed to be slow. `strategy/backtest.hpp` is kept **out** of `strategy.hpp`
on purpose: a backtest pulls in market_data, the execution partition and the
risk gate, and a translation unit that merely defines a strategy should pay for
none of that.

Almost everything it drives is the shipped path — the same `depth_feed_bridge`,
`risk_gate`, `engine_partition` and `EXCHANGE` a deployment runs. The
exceptions are the three files a diff-depth capture forces: `fill_model.hpp`
decides *whether* a resting order of ours would have been hit (a capture records
quotes rather than prints, so nothing in it will ever aggress against a passive
order), `queue_position.hpp` estimates *how much of it* traded given what was
queued in front, and `wire.hpp` holds a command for however long the run was
told a command takes. Those are the files to read before believing a number a
run produces, and the first two are argued out in
[backtesting.md](backtesting.md).

`depth_feed_bridge` is here rather than in `app/`, and that is the whole of the
reason: it is the single join between `market_data/` and ``, two
siblings that never link each other, so only a module above both may hold it.
The composition root qualified and was where it started — but every consumer
then had to reach *up* into `app/` for it, which is an edge pointing the wrong
way for the sake of a rule about who is *allowed* to name what. Beside its only
caller, both properties hold at once.

### risk_management/ — the risk gate and what watches after it

Sits **inline between a strategy host and the execution gateway** and screens
every command before the gateway sees it: order size, notional, a fat-finger
price band, net position, gross exposure, message rate, and a kill switch. Then
watches what comes back — messages per execution, executions per window, a
return path that has gone quiet — because those are the failures no single
command can be asked about.

```text
risk_management/
├── limits.hpp/.cpp         the policy an operator sets
├── window.hpp              bucketing a reading into fixed windows
├── gate.hpp                risk_gate — the inline check that composes the hooks
└── hooks/                  one file per interception point
    ├── breach.hpp          what a hook found wrong, as bits
    ├── observer.hpp        who is told when a hook fires
    ├── feedback.hpp        feedback_router — the return path into risk
    ├── detail/screening.hpp what a batch carries into the rules
    ├── detail/fixed_window.hpp the counting window the rate-shaped rules share
    ├── pre_trade/          per command, on the hot path
    │   ├── order_size_check.hpp   quantity, and price × quantity
    │   ├── price_collar.hpp/.cpp  the band, and the rule read against it
    │   ├── position_limit.hpp     net position and gross exposure, projected
    │   ├── position.hpp/.cpp      position_book — shared, atomic, cross-thread
    │   ├── rate_limiter.hpp/.cpp  the window, and the rule read against it
    │   ├── duplicate.hpp/.cpp     an id already working, and the ceiling
    │   └── working_ledger.hpp/.cpp orders in flight, gate-private
    ├── post_trade/         per published event, off the hot path
    │   ├── limits.hpp             thresholds, kept out of risk_limits
    │   ├── order_trade_ratio.hpp/.cpp messages per execution
    │   ├── fill_burst.hpp/.cpp    executions per window, and a price run
    │   ├── outcome_silence.hpp/.cpp exposure nothing comes back about
    │   └── monitor.hpp/.cpp       post_trade_monitor — one owner per listing
    └── system/             out-of-band, and they stop everything
        ├── global_kill_switch.hpp    what a tripped breaker refuses
        ├── circuit_breaker.hpp/.cpp  the switch all three of these trip
        ├── trading_state.hpp         what it says, and why it stopped
        ├── pnl_drawdown_breaker.hpp/.cpp the loss floor
        └── heartbeat.hpp/.cpp        silence from the venue
```

A library like every other module, not a pile of headers: it gets the generated
export header, the warning set and the hardening the rest of the tree runs
under.

**The directory listing is the list of checks.** An operator asking "what runs
before an order leaves" should be answered by `ls`, not by scrolling a
700-line class — the checks are a compliance artefact as much as a design. So
each rule is one file, and each file is one thing that can refuse an order.

**A hook is a function, not an object.** The obvious reading of "a chain of
hooks" is a vector of polymorphic checks called in a loop, which would cost an
indirect call and an unpredictable branch per rule on the path budgeted in
nanoseconds. Instead every rule is a free function returning the bits of what it
found broken, and `risk_gate::place_limits` calls all of them unconditionally and
ORs the results: ten rules, ten compares, one branch. Splitting them out of the
gate bought each one a name, a docstring and a test of its own without changing
an instruction. @see [hooks/fwd.hpp](../src/risk_management/hooks/fwd.hpp)

**The state a hook measures against lives with the hook.** The ledger the
duplicate rule reserves in, the window the rate rule reads, the position book the
exposure projection is measured against, the breaker all three system hooks trip.
Nothing under `hooks/` reaches back up the tree for the thing it is about, and the
module root is left holding only what every hook shares — the policy, the clock,
and the gate that composes them.

And the filing is spelled out at the call site. These types are deliberately
**not** aliased back into `exchange::risk`: a flat `risk::circuit_breaker` reads
as module vocabulary and hides which lane owns it, which is exactly what a
reader tracing dependencies needs to know. So it is
`hooks::system::circuit_breaker` from outside and `system::circuit_breaker` from
within `hooks`, and moving a component between lanes is a compiler-guided
rename — paid once, in exchange for an edge that is legible everywhere it is
used.

**Three lanes, three cadences.** `pre_trade/` answers "may this order go" and
runs per command inside `submit_range`. `post_trade/` answers "was what just
happened the right shape" and runs per published event on the dispatcher thread.
`system/` answers "may anything go" and runs on whatever schedule its driver
has. The last two both end in a `circuit_breaker` trip — the byte every
pre-trade screen already reads out of `screen_state` — which is why neither
costs the submit path anything and neither needs it to know they exist. It is
also why the drawdown breaker is not a pre-trade rule, and why an order-to-trade
ratio is not either: a losing position and a thousand-to-one quote ratio are
facts about history, not about the order in front of you, so refusing that order
while accepting the next identical one would be incoherent.

The three lanes are deliberately **not** three symmetric objects. `pre_trade/`
already has its composer in `risk_gate`, and that composer is branchless
arithmetic that belongs on the submit path. `system/` has no composer and wants
none: its hooks are driven by three unrelated signals and meet only in the
breaker they all write. `post_trade/` is the one lane with a single input, a
single owner and a loop — one listing's whole published event stream, in order,
on one thread — so it is the one lane that is an object. A uniform "hook engine"
interface across all three would have made the nanosecond path pay for the
generality of the millisecond one.

**The post-trade lane is driven from the return path that already exists.**
`hooks::feedback_router` is the only thing in the process that sees every
published event for every listing on one thread, which is exactly this lane's
input — so a monitor is attached beside the gate that screens its listing and
fed from the same two calls, and `router.poll()` drives the one rule whose
subject is the *absence* of events. A deployment that attaches no monitor pays
one null check per span and never reads a clock. What the lane can see bounds
what it may claim: a `trade` carries no side, so the adverse-run rule measures
the tape rather than pretending to know which side of a print was ours, and the
gate's `working_ledger` has no iteration, so the staleness rule measures
exposure-with-silence rather than the age of any one order. Both limits are
written down where the rule is. @see
[hooks/post_trade/fwd.hpp](../src/risk_management/hooks/post_trade/fwd.hpp)

**Implementation details live in `exchange::risk::hooks::detail`** and
`hooks::pre_trade::detail`. The bit-mask machinery behind `first_reason`, the
batch state the rules read, the open-addressed table under the ledger and the
padded entry under the position book are visible in headers because `constexpr`
and templates need them to be — the namespace is what says *visible, not
offered*.

It gets between the other two **without either naming it**: a gate models the
same structural `command_sink` a strategy host already writes through, so wiring
one in is a change to one line and there is no edge from `strategy/` to
`risk_management/` or back. The conformance is a `static_assert` in
[test/risk_management/gate/composition.test.cpp](../test/risk_management/gate/composition.test.cpp),
which is allowed to name both. Two gates stack — a per-strategy one inside a
per-desk one — for the same reason. The return path is the mirror image:
`hooks::feedback_router` models `event::event_handler`, so an `event_dispatcher`
feeds each listing's fills and outcomes back to the gate that screened them
without either module naming the other.

The checks do not short-circuit: each rule is evaluated into a register and its
bit ORed into a mask, so ten rules cost ten compares and one branch, and a
refused order costs the same as an accepted one. Measured numbers are in
[performance.md](performance.md#pre-trade-risk--measured-and-inside-budget).

`position_book` is the only shared state and the only place atomics appear. One
writer per symbol, many readers, all relaxed — so an update is `mov/add/mov`
with no `lock` prefix rather than a `fetch_add`. Everything else the gate owns
is single-threaded by construction.

### app/ — the composition root, and the live path

The only module allowed to name every other one, which is what makes it the
place two subsystems may be joined. Everything here is either a driver for one
subcommand or a piece of wiring that has exactly one caller.

```text
app/
├── cli.hpp/.cpp        one registrar per subcommand; options only, no logic
├── configuration.hpp/.cpp  defaults, then INI, then the command line
├── main.cpp            which commands exist, visible at the entry point
├── live_feed.hpp       the live ingest pipeline: frames plus REST repair
├── live_session.hpp    the live topology: feed, engine, two threads
├── latency_pipe.hpp    the modelled network, between the gate and the engine
├── feedback_fanout.hpp what one listing's published events are delivered to
└── commands/           one driver per subcommand
    ├── snapshot.cpp  capture.cpp  live.cpp  replay.cpp
    ├── trades.cpp    recover.cpp  backtest.cpp demo.cpp
    └── serve.cpp     the live path, and the only command that stays up
```

**Every offline driver goes through the feed seam, including `replay`.**
`replay.cpp` used to decode the whole capture with
`parse_binance_depth_updates` and hand each frame to `apply_depth_update` in a
loop. That was not a cheaper version of the reconstruction procedure; it was a
different and wrong one, because a capture must be started *before* its snapshot
is fetched — otherwise the frames bridging the two are lost — so the first
frames on the file always predate the seed. Their sizes are absolute and older,
so replaying them over the snapshot resurrected levels the venue had already
removed, and the loop had no sequencer with which to notice. On a real 180 s
BTCUSDT capture that is 73 of 1799 frames and ten phantom levels.

It now builds a `jsonl_depth_feed`, seeds it with the normalised snapshot and
drives it into a `depth_reconstructor`, so it discards what the snapshot covers,
counts overlaps, and — the part the old loop could not do at all — reports gaps.
`trades.cpp` is the same shape over `trade_feed`. The rule is that a driver
names a venue only where it builds the feed, never inside the loop.

**`live_session.hpp` is the live topology and `serve.cpp` is only its shim.**
The split is deliberate and it is about testability: everything about *what is
connected to what* — the depth bridge, the quoter, the risk gate in front of the
partition, the event channel coming back, the post-trade monitor and the feed
watchdog — is in the header, where
[test/app/live_session.test.cpp](../test/app/live_session.test.cpp) drives it
with scripted depth events and no socket. What is left in the `.cpp` is an
io_context, a signal handler, a consumer thread, two timers and a report: things
a test would gain nothing from asserting on. The second timer is the one that
delivers commands whose modelled flight time has elapsed — a session exposes
`deliver_due` and `next_due_ns` so a test can do that by hand, and so the only
thing left in the `.cpp` is the waiting. @see `latency_pipe`

**Two threads, and one member belongs to the second.** Every member of a
`live_session` is the producer thread's — the io_context's — except
`drain_and_publish`, which is the matching thread's and the only thing it may
touch. Commands cross one way through the partition's SPSC queue and events come
back the other way through an `event_channel`; nothing mutable is shared. The
one subtlety worth reading the header for is that every wait on the producer side
*pumps the channel*, because `event_channel` stops a consumer that cannot
publish, and a producer that waited without draining would deadlock against it.

**What `serve` is by default**: not an order gateway. The venue supplies prices
and depth; `depth_feed_bridge` seeds that depth into this process's own
`EXCHANGE` as anonymous liquidity, and the strategy trades against it there.
That is the bridge's stated purpose, and it is why the live strategy **takes**
liquidity by default: seeded depth is rested with `add_order`, which does not
match, so a resting quote has nothing to fill against — and the inference that
covers that offline (`crossing_fill_model`) reads the matching thread's own
records and cannot be run from the producer side. `--quote` still runs the
passive quoter, and honestly reports no fills.

**What `--send-orders` changes**, in both directions at once. Outbound,
`session::order_router` sits between the gate and `latency_pipe` and copies
every PLACE and CANCEL the gate passed into an outbox, signed by
`session::venue_gateway`; a coroutine in `serve.cpp` drains that over one
pipelined connection. Inbound, the venue's account stream reaches
`live_session::on_report`, which books fills through the same risk gate that
screened the order. Three things follow, and each is enforced rather than
documented:

- It needs `--quote`. `--take` crosses the touch *in this process's own book* —
  depth mirrored from the venue — so every order would fill once here and once
  at the venue. A resting quote cannot fill internally, which makes the venue's
  report the only execution there is.
- It refuses `--simulate-fills`, for the same reason from the other side.
- It refuses to run without `--testnet`, `--demo` or `--live`. The environment
  drives the depth stream, the snapshot fetch, the reference-data read and the
  order path from one value, because a run reading production depth while
  placing orders on testnet is a strategy reacting to a market it is not
  trading in. The default is production *for the feed*, and order entry is
  never allowed to inherit that silently.

What it deliberately does not do is force the engine's book to agree with the
venue's. The engine's book records what this process *decided*; the venue's
records what it *accepted*. `session::reconcile` is what compares them, and
`exchange_tool account` is what runs it.

**`live_feed.hpp` and `feedback_fanout.hpp` are here for the same reason
`depth_feed_bridge` is not.** A component with one caller belongs beside that
caller, and a component with two belongs below both: the bridge moved down into
`strategy/` when the backtest needed it, and `spread_quoter` moved out of
`backtest/` when `serve` became its second caller. These two have one caller
each, and they are in the composition root because nothing below could host
them — `market_data` links neither Boost nor `transport`, and `transport` must
not know what a depthUpdate is.

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

### simd/ — vector kernels over a price ladder

Two reads that a book asks about many levels at once, and nothing else:
`total()` sums a run of level sizes, `consume()` says how many levels a given
size clears. Both have `_interleaved` forms for a caller whose levels are
`{price, size}` pairs rather than a bare run of sizes, which is what
`market_data::l2_book` stores and what `l2_book::total_volume` and
`l2_book::sweep` call.

```text
simd/
├── ladder.hpp    the two kernels, as ordinary exported functions
├── ladder.cpp    their bodies, compiled once per instruction set
└── target.hpp    which instruction set the dispatch actually reached
```

**The kernels are out-of-line, and that is the design.** `ladder.cpp` is built
by [Google Highway](https://github.com/google/highway)'s `foreach_target.h`
into one body per instruction set — SSE2, SSSE3, SSE4, AVX2 and the AVX-512
family on x86, NEON and SVE on arm64 — and the first call resolves to whichever
the CPU actually has. One binary built on a developer machine therefore uses
AVX-512 on a venue host without being rebuilt, and the cost is one indirect
call per *ladder* rather than per level.

That per-call cost is also the limit: below roughly sixteen levels the dispatch
is more than the whole walk, and a ladder that short should be walked by hand.
`ladder.hpp` carries the measured table and the crossover.

**Highway rather than the alternatives**, and the toolchain matrix decided it.
`<experimental/simd>` is absent from the MSVC standard library and partial in
libc++; VcDevel/std-simd is GCC-only and is already what libstdc++ ships;
GSI-HPC/simd (the C++26 `std::simd` reference) is GCC/Clang-only. None is in
vcpkg, so all three would mean `FetchContent`. That left xsimd and Highway, and
Highway's run-time dispatch is the feature: this machine cannot execute AVX2 or
NEON at all, so hand-written or compile-time-selected kernels for those would
ship having never run.

Two things Highway imposes. `ladder.cpp` must stay out of unity batches — the
re-inclusion machinery needs it to be a translation unit of its own — and
nothing in it may be given an architecture flag on the command line, since
Highway attaches the per-target attributes itself. `core/CMakeLists.txt` marks
the first; the second is a rule for whoever edits the file.

Highway disables AVX-512 under MSVC (`HWY_BROKEN_MSVC`), so the same source
reaches AVX-512 from the MinGW, Clang and AppleClang presets and stops at AVX2
from `windows-msvc`. `simd::active_target()` is how a test or a benchmark says
which it got, so a green run cannot be mistaken for a wide one.

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
(`market_data/format.hpp`, `format.hpp`) rather than the domain
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
