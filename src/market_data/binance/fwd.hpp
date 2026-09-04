#pragma once

#include <cstdint>

namespace exchange::market_data::binance {

enum class depth_error : std::uint8_t;
enum class depth_speed : bool;
struct depth_parse_error;
struct stream_endpoint;
struct http_endpoint;

struct depth_snapshot;
struct depth_update;
struct depth_update_meta;
class depth_parser;
class jsonl_depth_feed;

} // namespace exchange::market_data::binance
