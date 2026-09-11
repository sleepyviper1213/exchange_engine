#pragma once
// Forward declarations for the rest submodule's public types. Prefer this over
// the full headers wherever a declaration suffices, so a translation unit that
// only holds a reference does not pull in Boost.Asio.

#include <cstdint>

namespace exchange::transport::rest {

// tls_verify is deliberately absent: it belongs to the enclosing transport
// namespace now that the WebSocket reader needs it too, and re-declaring it
// here would declare a *different* type that silently shadows it inside `rest`.
// @see transport/tls_verify.hpp
enum class method : std::uint8_t;
struct header;
struct request;
struct request_options;
struct failure;
struct pipeline_options;
struct pipeline_stats;
class request_pipeline;

} // namespace exchange::transport::rest
