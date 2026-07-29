# Performance targets

The throughput the engine is built to sustain, the per-message budget each
target implies, and the gap between those budgets and what the code does today.
For the component layout see [architecture.md](architecture.md).

## Targets

| Path | Target | Budget per item | Cycles @ 3 GHz |
|------|--------|-----------------|----------------|
| Market data ingestion | 10,000,000 msg/s | 100 ns | ~300 |
| Order book updates | 1,000,000 updates/s | 1 µs | ~3,000 |
| Order processing | 100,000 orders/s | 10 µs | ~30,000 |

These are **sustained steady-state** figures for one machine, measured at the
boundary of the subsystem that owns the path — not burst peaks and not
end-to-end wire-to-wire latency, which is tracked separately.

A "message" is one decoded feed frame (a `depthUpdate`, a trade print). A "book
update" is one `set_level` / level mutation; one message carries several, so
the two numbers are not a ratio of each other. An "order" is one inbound
client command through `MatchingEngine::submit`.

## What the budgets rule out

**Ingestion cannot be single-core.** 100 ns per message is roughly 300 cycles.
A Binance `depthUpdate` frame is ~200–600 bytes; simdjson On-Demand decodes
typical documents at ~1–3 GB/s per core, so a 400-byte frame costs on the order
of 150–400 ns in JSON decode alone — before scaling, sequencing, or applying a
single level. The 10M/s target is therefore an **aggregate across sharded
ingest cores**, and needs either:

- a binary feed (ITCH/SBE-style over the DPDK path) where decode is a struct
  overlay rather than a parse, or
- 4–8 ingest shards partitioned by symbol, each running its own parser and
  book set.

Both, for a JSON venue at full rate. This is a topology requirement, not a
micro-optimisation, and nothing in the current tree provides it — the
dispatcher that would fan symbols out is still a scaffold
([dispatcher.hpp](../src/trading-engine/execution/dispatcher.hpp)).

**No allocation on any of the three paths.** 300 cycles does not cover a
`malloc`. Neither does 3,000 once a cache miss or two is priced in. Every
per-message and per-update allocation has to become a pool, an arena, or inline
storage.

**Order processing is a latency target wearing a throughput costume.**
100K orders/s is 10 µs each — enormous next to the other two. A single
partition already clears that with room to spare. What actually matters on
this path is tail latency and determinism under load, so it is gated on p99.9
per-order matching time, not on average rate.

## Where the engine stands

Grounded in the current tree. None of this is measured yet — see
[Measurement](#measurement) — so treat the ranking as a cost model, not a
profile.

### Ingestion path — blocked on allocation

`depth_event` holds two `std::vector<book_level>`
([normalised.hpp:76-81](../src/market-data/normalised.hpp#L76-L81)), so every
normalised event allocates twice. At 10M msg/s that is 20M allocations per
second on the hottest path in the system. The levels need fixed-capacity inline
storage or an arena the event borrows a span of.

The buffered-replay path compounds it: `depth_reconstructor::retain` moves whole
`depth_event`s into a `std::deque`
([reconstructor.cpp:10-20](../src/market-data/reconstructor.cpp#L10-L20)).

`l2_book` itself is sound — contiguous `{price, volume}` cells, binary search
plus an in-place write, four levels per cache line
([l2_book.cpp:45-59](../src/market-data/l2_book.cpp#L45-L59)). It is the event
plumbing around it that allocates, not the book.

### Book updates — level representation done, index outstanding

`Level` now holds a `detail::order_list` — an intrusive FIFO of pool-index-linked
nodes over `core::memory::node_pool<resting_order>`
([level.hpp](../src/trading-engine/order_book/level.hpp),
[order_list.hpp](../src/trading-engine/order_book/detail/order_list.hpp)).
That removed four costs from the 1 µs budget at once:

- no heap allocation per new price level — a level takes pool slots, and the
  pool is reserved to the book's capacity hint up front;
- a fill unlinks the head node instead of `erase(begin())`, so it no longer
  memmoves the rest of the level;
- cancel is a hash lookup plus an O(1) splice, because `Location` now carries
  the node index alongside side and price — no scan for the matching id;
- `total_volume()` is an O(1) read of an incrementally maintained aggregate, so
  a fill-or-kill check is O(levels) rather than O(resting orders).

`Level` is also now a flat aggregate of scalars owning no storage, so the
`book_side` shift on level insert/erase is the same plain memmove `l2_book`
already gets right, rather than moving heap-owning vectors.

Still outstanding on this path: `std::unordered_map<order_id, Location> index_`
([order_book.hpp](../src/trading-engine/order_book/order_book.hpp)) costs a node
allocation per resting order and a pointer chase per cancel; it wants open
addressing or a direct slot handle. Narrowing `node_pool::Index` from 64 to 32
bits would also shrink `Level` enough to fit two per cache line.

### Order processing — one cheap fix outstanding

The partition path is structurally right: exclusive book ownership, a lock-free
SPSC ring with cached cursors on separate cache lines, batch enqueue, one
trade-sink call per drain.

`MatchingEngine::drain` is the exception — it pops one command at a time through
a `std::optional`
([matching_engine.hpp:86](../src/trading-engine/execution/matching_engine.hpp#L86)),
which the queue's own documentation warns against for hot loops. `consume_all`
and `try_dequeue_range` exist for exactly this and cost nothing to adopt.

## Measurement

A target is met when a repeatable benchmark says so, not when the code looks
fast. Rules:

- Every target above gets a benchmark that reports sustained throughput over a
  fixed replay corpus, with the ingest shard count stated in the result.
- Report p50 / p99 / p99.9, not just the mean — a throughput number that hides
  a 100 µs tail has not met the order-processing target.
- Pin threads and state the topology. An unpinned run measures the scheduler.
- Record the baseline before optimising, so a change can be attributed.

`benchmark/` already covers the pieces — `market_replay`, `matching_engine`,
`order_book`, `snapshot_load`, `parse_fixed_point`, and the SPSC queue
comparison suite. What is missing is an end-to-end gate per target and a
committed baseline to regress against.

## Order of work

1. ~~Wire `order_list` + `node_pool` into `Level` and flatten `book_side`.~~
   Done — see [Book updates](#book-updates--level-representation-done-index-outstanding)
   above. No baseline was captured beforehand, so the win is unquantified.
2. Batch `drain()` through `consume_all`. Smallest diff in the list.
3. Give `depth_event` inline or arena-backed level storage.
4. Replace `index_` with an open-addressed table.
5. Build the ingest shard fan-out (dispatcher, symbol partitioning, per-shard
   parser) — the only way the 10M/s figure is reachable at all.
6. Binary feed decode over DPDK for venues that offer one.
