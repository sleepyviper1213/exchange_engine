#pragma once

#include "market_data_export.hpp"

#include <cstdint>

namespace exchange::market_data::binance {

enum class depth_error : std::uint8_t;
enum class depth_speed : std::uint8_t;
struct depth_parse_error;
struct stream_endpoint;
struct http_endpoint;

struct DepthSnapshot;
struct DepthUpdate;
struct DepthUpdateMeta;
class DepthParser;

} // namespace exchange::market_data::binance
