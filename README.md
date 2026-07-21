# order_book

A low-latency, price–time-priority limit **order book and matching engine** in
modern C++ (C++23), fronted by a lock-free single-producer/single-consumer
command queue. It can be driven two ways:

- **L1 / matching flow** — identified orders (`place`, `cancel`) that cross and
  generate trades, with per-order FIFO priority.
- **L2 / market-data flow** — absolute-size level updates fed from a Binance
  `@depth` diff stream, reconstructing a live book without per-order identity.

Everything on the hot path avoids allocation, virtual dispatch, and locks:
contiguous sorted level vectors, an intrusive object pool, branchless search,
and a bounded lock-free ring.

---

## Architecture at a glance

```mermaid
flowchart LR
    subgraph Ingest["Ingest / producer thread"]
        FEED["Binance @depth feed<br/>(io::DepthParser)"]
        CLIENT["Client order flow"]
    end

    CMD["Command<br/>(tagged union)"]
    Q["spsc_queue&lt;Command, N&gt;<br/>lock-free ring"]

    subgraph Engine["Matching / consumer thread"]
        ME["MatchingEngine&lt;N&gt;"]
        OB["OrderBook"]
        BUF["trade buffer<br/>(reused)"]
    end

    SINK["TradeSink callback<br/>(batch of Trade)"]

    FEED --> CMD
    CLIENT --> CMD
    CMD -->|submit / submit_range| Q
    Q -->|drain: try_dequeue| ME
    ME -->|apply| OB
    OB -->|fills appended| BUF
    BUF -->|once per drain| SINK
```

The queue is the seam between threads. The **producer** builds `Command`s and
hands them off (`submit`) — no trades exist yet. The **consumer** later `drain`s
the queue, applies each command to the `OrderBook`, and for `PLACE` runs the
matcher, appending fills into a reused buffer; at the end of the pass it fires
the trade sink once with that batch.

---

## Layers

| Layer | Files | Responsibility |
|---|---|---|
| **Transport** | `concurrent_queue/spsc_queue.hpp` | Bounded, lock-free, lossless SPSC ring. Absolute never-wrapped cursors distinguish full from empty; `memcpy` fast path for trivially-copyable elements. |
| **Command** | `engine/command.hpp` | Trivially-copyable tagged union describing one book mutation. Built via named factories so the active union member always matches the tag. |
| **Engine facade** | `engine/matching_engine.hpp` | Owns the queue + book + trade buffer + sink. `submit`/`submit_range` (producer), `drain` (consumer). Spawns no threads — the caller owns both. |
| **Matching core** | `engine/order_book.hpp/.cpp` | Price–time-priority book: sorted level vectors, id→location index, matching, resting, cancel. |
| **Storage types** | `engine/level.hpp`, `engine/order_list.hpp/.cpp`, `memory/object_pool.hpp` | `Level` = price + `OrderList`; `OrderList` = intrusive FIFO over an `ObjectPool<RestingOrder>`. |
| **Domain types** | `engine/order.hpp/.cpp`, `engine/trade.hpp`, `engine/types.hpp`, `engine/side.hpp` | `Order` (input), `Trade` (output), `Price/Volume/OrderId`, `Side`. |
| **Search** | `engine/branchless_binary_search.hpp` | `branchless_lower_bound` used to locate a price level. |
| **Market data / IO** | `io/binance_depth.hpp/.cpp` | Parse Binance REST snapshots and `depthUpdate` diffs (integer-scaled, no floating point) into `DepthSnapshot` / `DepthUpdate`. |
| **Tools** | `tools/` | `snapshot_tool`, `ws_capture_tool`. |

---

## Class model

```mermaid
classDiagram
    class MatchingEngine~N~ {
        -spsc_queue~Command,N~ queue_
        -OrderBook book_
        -vector~Trade~ trades_
        -TradeSink on_trade_
        +submit(Command) bool
        +submit_range(range) bool
        +drain() size_t
        +book() const OrderBook&
    }

    class spsc_queue~T,N~ {
        +try_emplace(args) bool
        +try_emplace_range(range) bool
        +try_dequeue() optional~T~
        +consume_all(fn) size_t
    }

    class Command {
        +Type type
        <<union>> Order order
        <<union>> OrderId cancel_id
        <<union>> LevelChange level
        +place(Order)$ Command
        +cancel(OrderId)$ Command
        +add/reduce/set_level(...)$ Command
    }

    class OrderBook {
        -OrderPool pool_
        -vector~Level~ bid_levels_
        -vector~Level~ ask_levels_
        -unordered_map~OrderId,Location~ index_
        +place_order(Order) vector~Trade~
        +place_order(Order, vector~Trade~&) void
        +cancel_order(OrderId)
        +add_order / delete_order / set_level(...)
        +best_bid() / best_ask() optional~Price~
        -match(Order&, vector~Trade~&) Volume
    }

    class Level {
        +Price price
        +OrderList orders
    }
    class OrderList {
        +NodeIndex head
        +NodeIndex tail
        +Volume total_volume
        +push_back / pop_front / unlink(OrderPool&, ...)
    }
    class ObjectPool~RestingOrder~ {
        +allocate(args) index
        +deallocate(index)
        +get(index) Node&
    }
    class RestingOrder {
        +OrderId id
        +Volume volume
    }
    class Order {
        +OrderId id
        +Side side
        +Price price
        +Volume volume
        +OrderType type
    }
    class Trade {
        +OrderId aggressor
        +OrderId resting
        +Price price
        +Volume volume
    }

    MatchingEngine "1" *-- "1" spsc_queue : owns
    MatchingEngine "1" *-- "1" OrderBook : owns
    spsc_queue "1" o-- "*" Command : carries
    Command ..> Order : PLACE payload
    OrderBook "1" *-- "2" Level : bids + asks
    OrderBook "1" *-- "1" ObjectPool : node storage
    OrderBook ..> Trade : produces
    OrderBook ..> Order : consumes
    Level "1" *-- "1" OrderList : composes
    OrderList ..> ObjectPool : links nodes in
    ObjectPool "1" o-- "*" RestingOrder : pools
```

Two nested containers, deliberately different:

- **Levels within a side** live in a `std::vector<Level>` kept sorted by price
  (bids descending, asks ascending) so the best price is always `front()` and a
  price is found in O(log n) via branchless binary search.
- **Orders within a level** live in an intrusive FIFO (`OrderList`) whose nodes
  sit in a shared `ObjectPool`. Links are pool **indices, not pointers**, so
  growing the pool never dangles them, and cancel is an O(1) splice.

`RestingOrder` is the normalized form of an `Order`: just `{id, volume}`. Side,
price, and time-in-force are implied by *where* it rests, so they're not stored
again.

---

## Order lifecycle (a crossing buy)

```mermaid
sequenceDiagram
    autonumber
    participant P as Producer thread
    participant Q as spsc_queue&lt;Command&gt;
    participant E as MatchingEngine (consumer)
    participant B as OrderBook
    participant S as TradeSink

    P->>Q: submit(Command::place(buy))
    Note over P: hands off and moves on;<br/>no trades yet
    E->>Q: drain() → try_dequeue()
    Q-->>E: Command(PLACE)
    E->>B: place_order(order, trades_)
    B->>B: match() vs opposite side (FIFO per level)
    B-->>E: fills appended to trades_ ; remainder rested
    E->>S: on_trade_(trades_)  (once for the batch)
```

`place_order(order)` returns a fresh `vector<Trade>`; the engine uses the
allocation-free overload `place_order(order, trades_)` so a whole drained batch
accumulates into one reused buffer.

---

## Core design decisions

- **Price–time priority.** Best price via sorted-vector `front()`; time priority
  via per-level FIFO. Matching sweeps opposite levels while they cross, filling
  oldest-first.
- **Object pool over `new`/`std::list`.** Pre-reserved contiguous slab, O(1)
  allocate/deallocate through a free list, index links. No malloc on the hot
  path, cache-dense traversal, deterministic latency. Fixed capacity — growth
  would reallocate and dangle handed-out references, so it's forbidden.
- **Lock-free SPSC transport.** Bounded, non-blocking, and *lossless* (a push on
  a full queue fails rather than overwriting). Cursors are cache-line separated;
  each side caches the other's cursor to avoid atomic loads on the common path.
- **Trivially-copyable `Command`.** Lets the queue take its batch `memcpy` path
  and keeps the payload branch-predictable.
- **Two disjoint book modes.** The identified `place`/`cancel` flow models
  per-order identity and FIFO; the L2 `set_level`/`add`/`delete` flow collapses a
  price to a single anonymous aggregate. They must not be mixed on one book.
- **No hidden threads.** `MatchingEngine` is caller-driven; you own the producer
  and consumer threads, matching the queue's SPSC contract and keeping it
  testable.

---

## Directory layout

```
src/
  engine/
    types.hpp            Price / Volume / OrderId
    side.hpp             Side (BID/ASK) + opposed()
    order.hpp/.cpp       Order (incoming request)
    trade.hpp            Trade (execution output)
    order_list.hpp/.cpp  RestingOrder, OrderPool, OrderList (intrusive FIFO)
    level.hpp/.cpp       Level (price + OrderList)
    order_book.hpp/.cpp  Matching engine core
    command.hpp          Command tagged union
    matching_engine.hpp  Queue + book facade
    branchless_binary_search.hpp
  memory/
    object_pool.hpp      ObjectPool<T> + Node<T>
  concurrent_queue/
    spsc_queue.hpp       Lock-free SPSC ring
    fast_queue.hpp
  io/
    binance_depth.hpp/.cpp   Binance depth snapshot / diff parsing
  tools/               snapshot_tool, ws_capture_tool
test/                  GoogleTest suites (incl. matching_engine_test.cpp)
benchmark/             Google Benchmark + queue comparisons
```

---

## Build

CMake + vcpkg, presets in `CMakePresets.json` (e.g. `windows-mingw`,
`windows-msvc`). Configurations: `Debug`, `Release`, `RelWithDebInfo`,
`AddressSanitizer`, `ThreadSanitizer`.

```sh
cmake --preset windows-mingw
cmake --build build/windows-mingw --config Release --target order_test
```
