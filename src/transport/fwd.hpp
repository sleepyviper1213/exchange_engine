#pragma once
// Forward declarations for the transport module's public types. Prefer this
// over the full headers wherever a declaration suffices (e.g. to hold a
// pointer or reference), so translation units avoid pulling in the Boost.Asio
// and DPDK machinery the full headers require.
//
// Beyond the certificate policy below, only the DPDK receiver surface exposes
// named types; the rest/websocket/replay sources declare free functions over
// standard-library types alone, so they have nothing to forward declare here.

#include <cstdint>

namespace exchange::transport {

/// @brief Whether a TLS connection checks the peer it reached. Shared by both
///        REST senders and the WebSocket reader. @see tls_verify.hpp
enum class tls_verify : std::uint8_t;

#ifdef ORDER_BOOK_WITH_DPDK
namespace dpdk {
/// @brief One NIC receive queue and its NUMA-local mbuf pool. @see dpdk.hpp
struct receiver_config;

/// @brief A borrowed DPDK Ethernet frame handed to a @c packet_handler.
struct packet_view;

/// @brief One-thread DPDK RX endpoint. @see dpdk.hpp
class receiver;

} // namespace dpdk
#endif // ORDER_BOOK_WITH_DPDK

} // namespace exchange::transport
