#pragma once

#include <cstdint>

namespace exchange::market_data::binance {

enum class depth_error : std::uint8_t;
enum class depth_speed : bool;
struct depth_parse_error;

struct depth_snapshot;
struct depth_update;
struct depth_update_meta;
class depth_parser;
class depth_frame_decoder;
class jsonl_depth_feed;

enum class trade_error : std::uint8_t;
struct trade_parse_error;

struct trade_message;
class trade_parser;
class trade_frame_decoder;
class jsonl_trade_feed;

} // namespace exchange::market_data::binance
