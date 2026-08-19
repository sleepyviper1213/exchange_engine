#pragma once

// Declarations for the per-command hooks.
//
// The state each of these hooks measures against lives here beside it rather
// than one level up: the ledger is what the duplicate rule reserves in, the
// window is what the rate rule reads, the position book is what the exposure
// projection is measured against. Nothing under hooks/ then has to reach back
// up the tree for the thing it is about.
//
// The rules themselves are not declared here. They are free functions whose
// whole point is to be small, and a declaration would buy a caller nothing it
// does not get from the header that defines them.

namespace exchange::risk::hooks::pre_trade {

struct working_order;
struct ledger_take;
struct position_snapshot;
struct price_band;

class position_book;
class working_ledger;
class rate_limiter;

} // namespace exchange::risk::hooks::pre_trade
