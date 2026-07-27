#pragma once

// Replay transport: read recorded market data back from disk — the offline
// counterpart to the live rest/websocket sources. Protocol-agnostic: it yields
// raw bytes/lines (e.g. a JSONL capture from exchange::transport::ws::capture); parsing
// into domain types stays in market_data. Header-only, no link deps.


#include "transport_export.h" // TRANSPORT_EXPORT (generated)

#include <string>
#include <vector>

namespace exchange::transport::replay {


/**
 * @brief Read a JSONL capture as individual frames, one per non-empty line.
 * @param path Filesystem path to a JSONL file (e.g. a depthUpdate capture).
 * @return One string per non-empty line, in file order.
 */
TRANSPORT_EXPORT std::vector<std::string> read_lines(const char *path);

} // namespace exchange::transport::replay
