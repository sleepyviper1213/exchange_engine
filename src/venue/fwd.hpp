#pragma once
// Forward declarations for the venue module's public types. Prefer this over
// the full headers wherever a declaration suffices, so a translation unit that
// only holds a reference does not pull in simdjson.

#include <cstdint>

namespace exchange::venue {

enum class environment : std::uint8_t;
struct stream_endpoint;
struct http_endpoint;
enum class execution_status : std::uint8_t;
enum class execution_kind : std::uint8_t;
struct execution_report;
struct outbound_order;
struct outbound_cancel;
class weight_budget;

namespace binance {

struct hosts;
struct api_error;
struct symbol_filters;

} // namespace binance

} // namespace exchange::venue
