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
| Pre-trade risk check | inline, per command | 200 ns | ~600 |
| — position update | inline, per fill | 100 ns | ~300 |
| — limit validation | inline, per command | 50 ns | ~150 |

These are **sustained steady-state** figures for one machine, measured at the
boundary of the subsystem that owns the path — not burst peaks and not
end-to-end wire-to-wire latency, which is tracked separately — see
[Reaction time](#reaction-time--instrumented-no-baseline-yet) for the half of
that this process can see.

A "message" is one decoded feed frame (a `depthUpdate`, a trade print). A "book
update" is one level mutation — an `l2_book::set_level` on the reconstruction
side, an add or a reduce on the matching side; one message carries several, so
the two numbers are not a ratio of each other. An "order" is one inbound client
command through `engine_partition::submit`.

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
([dispatcher.hpp](../src/execution/dispatcher.hpp)).

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

Grounded in the current tree. Only the risk path is measured — see
[Measurement](#measurement) — so treat the rest of the ranking as a cost model,
not a profile.

### Ingestion path — blocked on allocation

`depth_event` holds two `std::vector<book_level>`
([normalised.hpp:128-129](../src/market_data/normalised.hpp#L128-L129)), so every
normalised event allocates twice. At 10M msg/s that is 20M allocations per
second on the hottest path in the system. The levels need fixed-capacity inline
storage or an arena the event borrows a span of.

The buffered-replay path compounds it: `depth_reconstructor::retain` moves whole
`depth_event`s into a `std::deque`
([reconstructor.cpp:10-20](../src/market_data/reconstructor.cpp#L10-L20)).

`l2_book` itself is sound — contiguous `{price, volume}` cells, binary search
plus an in-place write, four levels per cache line
([l2_book.cpp:45-59](../src/market_data/l2_book.cpp#L45-L59)). It is the event
plumbing around it that allocates, not the book.

### Book updates — level representation done, index outstanding

`price_level` holds a `detail::order_list` — an intrusive Boost FIFO of
pool-backed `detail::resting_order` nodes
([price_level.hpp](../src/order_book/price_level.hpp),
[resting_order.hpp](../src/order_book/detail/resting_order.hpp)).
That removed four costs from the 1 µs budget at once:

- no heap allocation per new price level — a level takes pool slots, and the
  pool is reserved to the book's capacity hint up front;
- a fill unlinks the head node instead of `erase(begin())`, so it no longer
  memmoves the rest of the level;
- cancel is a hash lookup plus an O(1) splice, because `detail::order_location`
  carries the node itself alongside side and level — no scan for the matching
  id;
- `total_volume()` is an O(1) read of an incrementally maintained aggregate, so
  a fill-or-kill check is O(levels) rather than O(resting orders).

`price_level` is also a flat aggregate of scalars owning no storage, so the
`book_side` shift on level insert/erase is the same plain memmove `l2_book`
already gets right, rather than moving heap-owning vectors.

The id index is **done** and this document asked for it: it was
`std::unordered_map<order_id, Location>`, a node allocation per resting order
and a pointer chase per cancel, and it is now
`boost::unordered_flat_map<order_id_t, detail::order_location>`
([EXCHANGE.hpp](../src/order_book/EXCHANGE.hpp)) — open addressing, which is
what [Order of work](#order-of-work) item 4 asked for. No before/after was
captured, so the win is unquantified in exactly the way item 1 already is.

Still outstanding: `detail::resting_order` is 32 bytes and pinned there by a
`static_assert`, which is two to a cache line — but nothing has measured what
that buys against the 48-byte layout it replaced.

### The ladder — flat array measured and rejected

`book_side` keeps its levels in an intrusive red-black tree, plus a
`boost::unordered_flat_map` that answers exact-price lookups without walking it
([book_side.hpp](../src/order_book/detail/book_side.hpp)). The standing
question was whether a flat price-sorted array should replace the tree, on the
reasoning that already wins for `l2_book`: contiguous cells, a branchless
binary search, and a memmove the prefetcher likes better than a pointer chase.

Measured with
[ladder_position.bench.cpp](../benchmark/order_book/ladder_position.bench.cpp),
which replays a trace of level *births and deaths* — the only events that move
a level — against the tree and against both flat orientations. The answer is
no, and not narrowly.

| | ns/op, run 1 | ns/op, run 2 | vs tree |
| --- | --- | --- | --- |
| intrusive tree | 35.5 | 40.9 | — |
| flat, touch at `back()` | 88.1 | 106.4 | 2.5–2.6x |
| flat, touch at `[0]` | 289.7 | 334.6 | 8.2x |

MinGW GCC 16 at `-O3`, i7-3770K, synthetic feed, ~1000 levels per side, 1298
level ops per pass. Absolute cost moved 15% between the two runs and the ratios
did not, which is why the ratio is the number quoted. The tree also carries
`by_price_` upkeep in that run which a flat side would drop — binary search
would be its own lookup — so the comparison charges the tree for work the array
does not do and the array still loses.

Two reasons, neither narrow:

- **A 1000-level array is ~16 KB, half of L1d.** Its binary search is then the
  same scattered dependent-load chain as the tree walk, not the tight L1 scan
  that makes small arrays win. The locality advantage is spent before the shift
  is paid for at all.
- **The shift is not small.** Levels are not born near the touch. The mode is
  rank 128–255, only 11% of ops land within the top 32, and the mean shift is
  155 cells — 2.5 KB moved per op, against a tree walk of ~10 dependent loads.

| rank from touch | 0 | 1–31 | 32–63 | 64–127 | 128–255 | 256–511 |
| --- | --- | --- | --- | --- | --- | --- |
| births | 21 | 53 | 31 | 116 | 352 | 76 |
| deaths | 67 | 8 | 30 | 116 | 352 | 76 |

Deaths do cluster at the touch — the market eating the best level — which is
the whole reason the `back()` orientation beats `[0]` by 3.3x: draining the
touch is a `pop_back`. It is not close to enough.

A head index shifting whichever side is shorter buys nothing on top of that:
it measured *identical* to shifting toward the touch (155.3 cells mean, 310
p99, both), because every birth lands nearer the touch than the far end. So the
`back()` row is already the best flat layout available for this workload rather
than a strawman, and there is no third arrangement left to try.

Pointer stability is **not** what decides this, though `book_side.hpp` has long
implied it is: an array of `{price, level*}` handles would leave levels in
their pool untouched. Stability only rules out storing levels *by value*, whose
move would take their orders' list heads with it. The argument that survives
being pushed on is the cache footprint above.

`market_data::l2_book` is flat for the reason this is not: 128 levels is a few
cache lines, which is the regime where the shift is cheap and the scan stays
tight. The two books differ in layout because they differ in depth.

Unmeasured: a real venue feed. `replay.fixture.hpp`'s `SYNTH_WINDOW` confines
every synthetic diff to 200 ticks around the touch, so the rank distribution
above is the generator's assumption rather than a market's, and the ratio would
move with it. Point `OB_SNAPSHOT` and `OB_REPLAY` at a capture to redo it. The
two structural reasons do not depend on the feed, which is why the conclusion is
recorded here rather than left open.

### Bulk depth reads — vectorised, measured

Two reads walk a whole side rather than one level: summing the size resting on
it, and asking how far a given size reaches into it. Both now go through
`core::simd` ([ladder.hpp](../src/core/simd/ladder.hpp)), whose bodies Google
Highway compiles once per instruction set and dispatches at run time.
`l2_book::total_volume` and `l2_book::sweep` are the callers.

Measured with [ladder.bench.cpp](../benchmark/core/simd/ladder.bench.cpp),
MinGW GCC 16 at `-O3` with LTO on an i7-3770K — which reaches SSE4, two `int64`
lanes, and cannot execute AVX2 — against the hand-written scalar loop, in ns:

| levels | 8 | 32 | 128 | 512 | 4096 |
| --- | --- | --- | --- | --- | --- |
| `total` scalar/simd | 2.7 / 7.9 | 10 / 9.7 | 37 / 20 | 93 / 63 | 614 / 436 |
| `total_interleaved` | 7.0 / 8.0 | 29 / 13 | 87 / 30 | 296 / 99 | 2434 / 1380 |
| `consume` | 7.0 / 10 | 31 / 20 | 112 / 59 | 423 / 193 | 3290 / 1439 |
| `consume_interleaved` | 11 / 12 | 36 / 24 | 122 / 70 | 444 / 222 | 3465 / 1913 |

1.7x to 2.9x at `l2_book`'s default depth of 128, and a **loss** below about
sixteen levels where the dispatched call costs more than the whole walk. Three
things are worth reading out of that table rather than the headline:

- The `total` row is vector against vector. GCC auto-vectorises and unrolls a
  bare accumulation loop by itself, so the scalar bar is already using SSE; the
  margin is run-time dispatch reaching a register the compiler was not allowed
  to assume. On a machine whose baseline and dispatched target are the same
  width, expect that row to narrow.
- The `_interleaved` rows have no such competition. An optimiser will not turn
  a strided read of `{price, size}` cells into a de-interleaving load, which is
  why the AoS shape `l2_book` argues for gains the most (2.9x at 128) without
  changing its layout.
- The 4096 column falls off because 4096 cells is 64 KB and L1 is 32 KB. These
  become memory-bound before they run out of arithmetic, which is the usual
  ceiling and the reason the retained window matters more than the kernel.

Unmeasured here: AVX2, AVX-512 and NEON. This machine has none of them, so
those paths are compile-verified and dispatch-verified only, and the CI runners
are where those numbers have to come from.

### Pre-trade risk — measured, and inside budget

The one path here with numbers rather than a cost model. `risk_gate` sits inline
between a strategy host and the gateway
([gate.hpp](../src/risk_management/gate.hpp)), so every command pays it.

Measured with `exchange_bench` on two machines. **A** is x86-64, 8 × 3510 MHz,
32 KiB L1d / 256 KiB L2 / 8 MiB L3, `windows-msvc-release`. **B** is an Apple M1,
8 cores, 64 KiB L1d / 128 KiB L1i / 4 MiB L2, Homebrew LLVM release. Both
`--benchmark_min_time=2s`; A is the mean of 5 repetitions.

Run the risk family **on its own**, not as part of the whole suite — see
[Measurement](#measurement) for why the full-suite figures are 1.5–2.5× worse and
must not be quoted.

| Benchmark | A (x86-64) | B (M1) | Budget |
|---|---:|---:|---|
| `BM_LimitsInspectPass` | 14.9 ns | 6.85 ns | 50 ns |
| `BM_LimitsInspectBreach` | 15.4 ns | 6.87 ns | 50 ns |
| `BM_PositionApplyFill` | 2.02 ns | 3.85 ns | 100 ns |
| `BM_PositionRead` | 4.98 ns | 1.58 ns | 100 ns |
| `BM_PositionSnapshot` | 2.96 ns | 1.59 ns | 100 ns |
| `BM_RateHeadroom` | 2.29 ns | 0.51 ns | — |
| `BM_LedgerInsertRetire` | 11.5 ns | 4.11 ns | — |
| `BM_GateSubmitBatch/1` | 74.8 ns/order | 44.8 ns/order | 200 ns |
| `BM_GateSubmitBatch/16` | 42.8 ns/order | 17.4 ns/order | 200 ns |
| `BM_GateSubmitBatch/64` | 41.2 ns/order | 20.2 ns/order | 200 ns |
| `BM_GateSubmitBatch/256` | 43.8 ns/order | 17.8 ns/order | 200 ns |

B is roughly 2.2× faster across the board, which matches the ratio of the pure
arithmetic — so the difference is machine and toolchain, not anything
algorithmic. `BM_PositionApplyFill` is the one inversion and is discussed below.

**What an observer costs, and why the answer is "nothing measurable".** A gate
with a logging observer attached is the configuration `serve` runs, and it is the
one case where a hook is called per command rather than never:
`risk_gate::commit` calls `notify_breach` once per *refused* command. So the
refusal path is the only place an observer can be priced - the accepted path
calls no hook at all, because every one is wrapped in `if constexpr`.

`BM_GateRefuseBatch_NoObserver` and `BM_GateRefuseBatch_Logging` screen a batch
in which every command breaks `ORDER_QUANTITY`. On machine A,
`windows-mingw-release`, 15 repetitions at a batch of 64:

| | median | stddev | cv | `gate_bytes` |
|---|---:|---:|---:|---:|
| `no_observer` | 1326 ns | 47.3 ns | 3.55% | 360 |
| `app::gate_logger` | 1339 ns | 29.1 ns | 2.17% | 368 |

The mean delta is 12 ns across 64 refusals - 0.19 ns each - against standard
deviations of 47 and 29 ns. That is a quarter of one deviation, so **no per-command
cost is resolvable here and none should be quoted**; what the pair establishes is
a bound (well under a nanosecond per refusal) and the absence of anything
surprising. The one deterministic figure is the counter: the observer adds one
pointer, taking the gate from 360 to 368 bytes, and it lands in the slot
`[[no_unique_address]]` was keeping free for an empty one.

Two design choices are what hold it there, and both would show up in this pair if
they were removed: the level check is inline in the header while the formatting is
out of line, so a run at `info` early-outs without a call; and every other gate in
the tree instantiates `no_observer`, so `if constexpr` deletes the call site
rather than guarding it.

**What this table can and cannot settle.** Within one run the coefficient of
variation is 1–11%. *Between* runs on machine A it is worse: `BM_PositionRead`
has been observed at 4.98 and 7.07 ns across two runs with no code change
touching it. So a 3× margin against a budget is a safe conclusion and a 10%
difference between two builds is not — anything smaller than about 30% needs a
quieter machine before it means anything. Do not use these numbers to justify a
micro-optimisation; use them to answer whether a budget is met.

"Full round trip" is submit through to the fill that retires the order: screen,
ledger insert, deliver to the sink, commit, then `on_trade` moving the position
and freeing the working quantity. Real `steady_clock` throughout.

Three things the table says that the code alone does not:

**The rules cost the same whether they pass or fail** — within about 1 ns on
either machine, for an order that breaks nothing and one that breaks three rules
at once, and the sign of the difference flips between runs. That is the
branchless mask working: there is no fast path to fall into, so an adversarial
order stream cannot make the check slower than a benign one, and the measured
case *is* the worst case. Short-circuiting `if`s would have made these two
numbers differ consistently and made the bad one unbounded. Two ISAs and two
compilers agreeing on it is much stronger evidence than either alone.

**Batching is where the fixed cost goes.** The clock read, the breaker load and
the position read happen once per `submit_range`, not once per command, so the
per-order cost falls 63 → 38 ns between a batch of one and a batch of 64 and
then flattens. `strategy_engine` flushes 16 events at a time, which is already on
the flat part of that curve.

**`enable_hardening` costs more than the loads it guards, on MSVC.**
`BM_PositionSnapshot` does *six* loads behind *one* live `assert` and is
consistently faster than `BM_PositionRead`, which does three loads behind three
— 2.96 against 4.98 ns here, 3.06 against 7.07 in an earlier run. The ratio moves
but the ordering does not, so the bounds check rather than the load is what
dominates. Clang shows no gap at all (1.59 vs 1.58 ns), making this an MSVC
codegen characteristic rather than a universal one. `open_batch` takes one
snapshot for that reason; the effect on the gate as a whole is below what
machine A can resolve, so no number is claimed for it.

#### Tails, which is what the budgets are actually written in

Means cannot answer a latency budget. `latency.bench.cpp` times individual calls
through the cycle counter in [latency.fixture.hpp](../benchmark/latency.fixture.hpp),
pinned, and reports the distribution. Machine A, `--benchmark_min_time=3s`,
`pinned=1`, harness noise floor `clock_ns=14.8`:

| Benchmark | p50 | p99 | Budget | |
|---|---:|---:|---:|:--|
| `BM_GateLatency_LimitCheck` | 20.8 ns | 43.6 ns | 50 ns | pass |
| `BM_GateLatency_PositionApply` | <1 ns | 6.3 ns | 100 ns | pass |
| `BM_GateLatency_PositionRead` | 7.1 ns | 14.0 ns | 100 ns | pass |
| `BM_GateLatency_SubmitBatch/16`, per order | 18.7 ns | 41.3 ns | 200 ns | pass |
| `BM_GateLatency_SubmitBatch/64`, per order | 17.5 ns | 37.1 ns | 200 ns | pass |
| `BM_GateLatency_Submit`, batch of one | 116.8 ns | 389.1 ns | 200 ns | **fails at p99** |

**Batching is not an optimisation here, it is the difference between meeting the
budget and missing it.** A batch of one puts the clock read, the breaker load and
the position read on a single order, and its p99 is 389 ns — nearly twice the
budget. At the batch size `strategy_engine` actually flushes, the same work is
41 ns per order at p99, five times inside. Anything driving the gate one command
at a time is outside what has been measured to work.

`BM_GateLatency_SubmitNoClock` isolates how much of that is Windows: p50 84.9 and
p99 315.9 against 116.8 and 389.1 with the real clock, so `steady_clock::now()`
contributes about 32 ns at p50 and 73 ns at p99. It is read once per
`submit_range`, so that whole cost divides by the batch size.

**What a quantile here means.** The sample at zero-based rank `floor(q * n)`,
clamped into range — so p99 of 100 samples is the 100th, not the 99th. One rule,
in [src/core/metrics/quantile.hpp](../src/core/metrics/quantile.hpp), used by
the benchmark harness and by the engine's own `histogram` alike. That is worth
stating because it was not always true: the harness used to take rank
`floor(q * (n - 1))`, one lower, so a p99 measured here and a p99 reported by a
running engine were answers to different questions. Figures in this document
recorded before that was unified may sit one rank low at p99 and p99.9.

**Read p99.9 and max with suspicion.** Only the *median* harness overhead is
subtracted, so the upper tail carries the clock's own tail on top of the code's.
At a 14.8 ns floor against a 50 ns budget the harness is a third of the target.
`max_ns` lands in the tens to hundreds of microseconds on every family including
the trivial ones, which is the OS preempting a pinned thread — it is not the
code, and it is the reason a real tail claim needs an isolated core rather than
just an affinity call. The boot parameters that produce one, and how the process
reports whether it got one, are in
[docs/deployment.md](deployment.md#kernel-isolation); none of the figures in
this document were taken on an isolated core, which is why every tail here
should be read as an upper bound that the scheduler contributed to.

**The one inversion is `BM_PositionApplyFill`**, where x86-64 is 1.8× *faster*
(2.18 vs 3.85 ns) against a 2.2× deficit everywhere else. It is the most
store-bound loop in the set — three relaxed read-modify-writes on one cache line
with a compiler barrier every iteration, so nothing stays in a register and each
iteration reloads what it just stored. x86's store buffer and store-to-load
forwarding appear to handle that pattern better than the M1's. Both are far
inside budget, so this is a curiosity rather than a problem, but it is the one
place where the single-writer relaxed load/store idiom is not uniformly cheap
and it should be re-checked in the generated code before being relied on.

### Order processing — measured, and 38x inside budget

Machine A (8 x 3510 MHz, 32 KiB L1d / 256 KiB L2 / 8 MiB L3),
`windows-msvc` Release. An "item" is one inbound command through
`engine_partition::submit`, which is what the 10 us budget is written in.

| | wall ns/item | budget | |
|---|---|---|---|
| `BM_MatchingEngine_MatchThroughput/4096` | 239 | 10 us | 42x inside |
| `BM_MatchingEngine_MatchThroughput/32768` | 261 | 10 us | 38x inside |
| `BM_MatchingEngine_QueueThroughput/32768` | 68 | — | queue + dispatch, no matching |
| `BM_MatchingEngine_TwoThreadPipeline/262144` | 262 | 10 us | 38x inside |
| `BM_CrossOneLevel/price_time/1024` | 17.9 /fill | — | |
| `BM_CrossOneLevel/pro_rata/1024` | 41.5 /fill | — | pro-rata divides, price-time walks |

So matching is ~260 ns against a 10 us budget, of which ~68 ns is the queue and
drain loop and the rest is the book. Small ranges are not comparable: at
`/8` and `/16` the per-pass `order_manager::clear` and the sink call dominate
and the figure is 3x worse, which says nothing about the steady state.

**Read the wall column, not CPU.** Google Benchmark derives `items_per_second`
from CPU time, and on Windows that comes off `GetProcessTimes` at ~15.6 ms
granularity: divided across iterations it quantises, and it disagrees with
itself in both directions - `price_time/512` reports 23.4 ns/fill by CPU and
`/1024` reports 14.3, where the wall column gives 17.8 and 17.9. The wall
figures are self-consistent across four sizes; the CPU ones are not.

**No before/after for the record widening.** TODO.md #7 grew `trade` from 24 to
56 bytes and `order_outcome` from 24 to 40, which roughly doubles the bytes
written per fill (136 against 72) and doubled `event_channel`'s inline ring
from 256 KB to 512 KB - past L2, and an eighth of L3. The table above is the
*after* run and there is no matching-path baseline recorded before it, so the
delta is unmeasured and no claim is made about it. What the table does settle
is whether it can matter yet: at a 38x margin it cannot. This section is the
baseline the next such change measures against.

The gate table further down is **not** usable as a before, either. Several
benchmarks there that cannot touch either record - `BM_RateHeadroom`,
`BM_PositionRead`, `BM_LedgerInsertRetire` - come out at roughly twice their
recorded figures in the same run, so that run and the recorded one differ for
reasons unrelated to any code change. Comparing across them would attribute
machine conditions to a diff.

### Order processing — and why the cheap fix is not available

The partition path is structurally right: exclusive book ownership, a lock-free
SPSC ring with cached cursors on separate cache lines, batch enqueue, one
trade-sink call per drain.

`engine_partition::drain` still pops one command at a time through a
`std::optional` ([engine_partition.hpp](../src/execution/engine_partition.hpp)),
which the queue's own documentation warns against for hot loops. This document
used to call adopting `consume_all` the smallest diff in the list. **It is not
available at all**, and the reason is worth stating so nobody spends an
afternoon rediscovering it.

`consume_all` and `consume_up_to` invoke the caller's callback *inside* a
`noexcept` member, between reading each element and destroying it, before the
read cursor is published. Their precondition is therefore that the callback
neither throws nor allocates. The callback here would be `matching_engine::
process`, which appends fills and lifecycle records to two `std::vector`s — so
it allocates whenever a batch outgrows the reserve, and a `bad_alloc` crossing
that boundary is a `std::terminate` with the ring half-drained. The queue is
right to forbid it; the drain loop is right to apply commands outside the queue
call, which is what the journalled branch beside it already does.

What is available, and cheap, is `try_dequeue(T&)` — the queue's own
recommendation for hot loops, and it avoids the per-command `optional`. It needs
`event::command` to be default-constructible, which it is not: every
constructor takes a tag and a payload precisely so a command cannot exist
without one. Giving it a default constructor to save an `optional` would trade
a real invariant for a small one, so the `optional` stays until someone
measures it costing something.

Both drain loops are now bounded by `QueueCapacity` rather than by the queue
running dry. That is not an optimisation: the producer is a different thread and
may be refilling as the consumer drains, so an unbounded loop can grow the
reused batch buffers without limit — a `malloc` on the matching path, which is
the one thing this file may not do.

### Production systems — metrics, measured

`core/metrics` (`counter`, `histogram`, `registry`, `text_exposition`) gives
`engine_partition` an optional `partition_metrics*` — see
[directory_layout.md](directory_layout.md) and the class comments in
[engine_partition.hpp](../src/execution/engine_partition.hpp).
Measured with `exchange_bench` on machine A (see [Pre-trade risk](#pre-trade-risk--measured-and-inside-budget)
for the full spec), `--benchmark_repetitions=5 --benchmark_report_aggregates_only=true`,
mean of 5 reps. Run the `Metrics` and `EnginePartitionLatency` families **on
their own**, not as part of the whole suite — see [Measurement](#measurement).

**The primitives, in isolation:**

| Benchmark | Mean | Budget |
|---|---:|---|
| `BM_MetricsCounter_Add` | 1.79 ns | — |
| `BM_MetricsHistogram_Record` | 3.18 ns | — |

And as a distribution (`latency.fixture.hpp`, pinned, `clock_ns` floor 11.1 ns):

| Benchmark | p50 | p99 | p999 |
|---|---:|---:|---:|
| `BM_MetricsLatency_CounterAdd` | ~1 ns | 11.3 ns | 14.4 ns |
| `BM_MetricsLatency_HistogramRecord` | ~1 ns | 13.4 ns | 20.4 ns |

Both p50s sit at or below the harness's own clock floor — the true cost is
below what this clock can resolve, which the ~2–3 ns loop-mean figures above
already imply. Read against the same caveat as the risk-gate latency table:
a mean this close to `clock_ns` is measuring the clock as much as the code.

**Cost on the path that actually uses them** — `engine_partition::drain()`,
batch of 64 crossing commands, with vs. without a `partition_metrics*`:

| | p50 | p99 | p999 |
|---|---:|---:|---:|
| No metrics | 1182 ns | 2707 ns | 10686 ns |
| With metrics | 1234 ns | 2616 ns | 10556 ns |

The "with" column is not uniformly worse — p99 and p999 both landed *lower*
than the unmetered run, which is noise (run-to-run stddev on p50 alone was
25–140 ns across the five reps), not a real speedup from adding
instrumentation. The honest reading is that one `steady_clock` pair, three
`counter::add`s and one `histogram::record` per drained batch are not
resolvable against this machine's noise floor at this batch size — inside
budget with room to spare, the same conclusion the risk-gate table reaches
for its own batched fixed costs.

**The production histogram against the ground truth.** Every drain the
sampler above timed with RDTSC was *also* timed by `partition_metrics`
through the real `scoped_timer` in `engine_partition::drain()`, so its own
`histogram::snapshot` can be read back and compared: `p50=2047 ns,
p99=4095 ns, p999=16383 ns` over the run's accumulated samples, against the
sampler's exact `p50=1234 ns, p99=2616 ns, p999=10556 ns`. The bucketed
figures are all *higher* than the exact ones — expected, since
`histogram.hpp` reports a bucket's upper bound, not the true value, trading
that for O(1) bounded memory on an always-on path (see its header comment).
2047 as the answer for a true p50 of 1234 is the coarseness that trade buys:
right octave, not the exact value, and that gap is constant per octave, not a
bug to chase.

**Against the ask's headline numbers.** The `<2 µs` / `10M msg/s` / `<50%
CPU` targets this work was asked to validate are the same three this document
already tracks — `10M msg/s` aggregate ingestion and true wire-to-wire
end-to-end latency, not any single component in isolation — and metrics
recording does not move them either way. What is shown above is that adding
metrics costs single-digit nanoseconds per primitive and is noise-level on
the batched drain path; it says nothing about the sharded ingest dispatcher
or binary feed decode that [Order of work](#order-of-work) already lists as
the only way `10M/s` becomes reachable, and CPU utilisation on dedicated
cores was not profiled here. Claiming otherwise would repeat exactly the
mistake [Measurement](#measurement) warns against — a number without the
scope it was measured under.

### Reaction time — instrumented, no baseline yet

Every other number in this document is a *component* number, and that is what
makes each of them defensible and all of them jointly silent about the thing a
trading system is judged on. A venue publishes a change; some number of
microseconds later this process has an order on the way, or has decided not to
send one. That interval is what "reacting 10 µs late means you are already
behind" is about, and until now nothing in the tree could express it — a
`depth_event` carried only `event_time`, the venue's own clock, from which no
local interval can be derived.

**What is now measured.** `core::chrono::ingress_time` is a stamp on the local
monotonic clock, taken at the transport edge and carried on the event
([ingress.hpp](../src/core/chrono/ingress.hpp)). `live_session` closes the
interval after the commands that frame caused have crossed into the partition's
queue, and records it into a `session::reaction_metrics` histogram
([reaction_metrics.hpp](../src/session/reaction_metrics.hpp)), exposed by `serve`
as `feed_frame_reaction_ns` when `--metrics-enabled` is set.

Inside the interval: the coroutine resume after the socket read, simdjson's
decode, normalisation, the sequencer's verdict, `l2_book`'s writes, the bridge's
ADD/REDUCE, the quoter, the risk gate, and the SPSC enqueue — including any
back-pressure retry, which is the most likely reason a reaction was slow.

Outside it, and not measurable from in here: the venue's matching-to-publish
delay, the wire, and the kernel receive path up to the point Beast handed over a
frame. The first two need the venue's clock and the third needs the NIC's, so
this is **ingress-to-egress** — the half of wire-to-wire this process is
responsible for and the only half it can improve. The DPDK path already takes a
hardware-adjacent `rte_rdtsc()` into `transport::packet_view`
([dpdk.cpp](../src/transport/dpdk.cpp)); carrying *that* up in place of the
software stamp is what would close the remaining gap, and it is not done.

**Resyncs are a separate distribution**, `feed_resync_reaction_ns`, and the
reason is the one thing here that is easy to get wrong. A snapshot that bridges
buffered events puts them into the book too, and the oldest of those arrived a
REST round trip ago — so the interval starts at *its* ingress, not the
snapshot's, via `depth_reconstructor::last_replay_ingress`. Measuring from the
snapshot understates a stalled resync by three orders of magnitude; that figure
is not an estimate, it is what the mutation test measured (131 µs against a true
50 ms+). Pooled into one histogram these samples would also move the p99 of the
metric whose p99 is the point, which is why they are kept apart.

**No numbers yet, and no budget.** The histograms are wired and tested but no
run has been recorded, so this section states what is instrumented rather than
what it costs — the distinction [Measurement](#measurement) insists on. The
histograms are constructed with no `latency_budgets`, which means always-healthy
rather than falsely-alarming until a baseline exists to set one from. Two
`steady_clock` reads per frame is nothing against a 100 ms diff stream and would
be unaffordable on a 100 ns-per-message binary feed, which is why the whole path
is behind a null pointer and off by default.

**What a replay cannot tell you.** `jsonl_depth_feed` deliberately leaves events
unstamped: a capture's frames arrived when they were recorded, so stamping them
on read-back would report the replay loop's own speed as a reaction time — a
number that improves the further the harness drifts from the live path it stands
in for. Unstamped messages are counted in `feed_unstamped_messages` rather than
silently skipped, so an empty distribution cannot be misread as a fast run.

## Measurement

A target is met when a repeatable benchmark says so, not when the code looks
fast. Rules:

- Every target above gets a benchmark that reports sustained throughput over a
  fixed replay corpus, with the ingest shard count stated in the result.
- Report p50 / p99 / p99.9, not just the mean — a throughput number that hides
  a 100 µs tail has not met the order-processing target.
- Pin threads and state the topology. An unpinned run measures the scheduler.
- Record the baseline before optimising, so a change can be attributed.
- **Run the family you are measuring, not the whole binary.** `exchange_bench`
  contains multithreaded benchmarks that saturate every core for minutes; a
  single-threaded family that runs after them measures a hot, contended machine.
  Observed on the risk family: 15.8 ns filtered against 31.2 ns in a full-suite
  run, with every other single-threaded benchmark inflated by a similar factor.
- **Check wall against CPU before believing a number.** A single-threaded
  benchmark should show them equal. The filtered risk run gives 15.8 / 15.6; the
  full-suite run gives 31.2 / 22.5, and that 1.4× gap is the process being
  descheduled, not the code being slow.
- Use `--benchmark_repetitions=5 --benchmark_report_aggregates_only=true` and
  quote the mean with its coefficient of variation. A single sample cannot tell
  a regression from a noisy afternoon.
- **A latency budget is answered with a percentile, not a mean.** Google
  Benchmark aggregates over repetitions of a whole loop, so one slow call
  disappears into the millions around it. [latency.fixture.hpp](../benchmark/latency.fixture.hpp)
  times individual calls against the cycle counter, subtracts its own median
  overhead, pins the thread, and publishes `p50_ns` / `p99_ns` / `p999_ns` plus
  the `clock_ns` floor to read them against. Check the `pinned` counter: a run
  that silently failed to pin is measuring the scheduler.
- Sampled and loop-average numbers are different questions and will not match.
  The sampler fences around every call, so consecutive iterations cannot overlap;
  `BM_GateLatency_LimitCheck` reports a p50 of 20.8 ns where the loop mean is
  14.9 ns. The first is one isolated call's latency, the second is the
  throughput-limited cost with overlap. Quote whichever the target asks for.

`benchmark/` already covers the pieces — `market_replay`, `matching_engine`,
`EXCHANGE`, `snapshot_load`, `parse_fixed_point`, `risk/gate`, and the SPSC
queue comparison suite. What is missing is an end-to-end gate per target and a
committed baseline to regress against. The risk table above is the first such
baseline; it names the machine because a number without one is not a baseline.

## Order of work

1. ~~Wire `order_list` + `node_pool` into `Level` and flatten `book_side`.~~
   Done — see [Book updates](#book-updates--level-representation-done-index-outstanding)
   above. No baseline was captured beforehand, so the win is unquantified.
2. ~~Batch `drain()` through `consume_all`.~~ Withdrawn — `consume_all`'s
   callback must not allocate and `matching_engine::process` does. See
   [Order processing](#order-processing--and-why-the-cheap-fix-is-not-available).
3. Give `depth_event` inline or arena-backed level storage.
4. ~~Replace `index_` with an open-addressed table.~~ Done —
   `boost::unordered_flat_map`. No before/after captured, so unquantified.
5. Build the ingest shard fan-out (dispatcher, symbol partitioning, per-shard
   parser) — the only way the 10M/s figure is reachable at all.
6. Binary feed decode over DPDK for venues that offer one.
7. Record a reaction-time baseline from a pinned `serve` run, then set the
   `latency_budgets` the histograms currently go without. @see
   [Reaction time](#reaction-time--instrumented-no-baseline-yet)
