#pragma once
// Forward declarations for the transport module's public types. Prefer this
// over the full headers wherever a declaration suffices (e.g. to hold a
// pointer or reference), so translation units avoid pulling in the Boost.Asio
// and DPDK machinery the full headers require.
//
// Only the DPDK receiver surface exposes named types; the rest/websocket/replay
// sources declare free functions over standard-library types alone, so they
// have nothing to forward declare here.
#include "transport_export.h" // TRANSPORT_EXPORT (generated)

#ifdef ORDER_BOOK_WITH_DPDK

namespace exchange::transport::dpdk {

/// @brief One NIC receive queue and its NUMA-local mbuf pool. @see dpdk.hpp
struct receiver_config;

/// @brief A borrowed DPDK Ethernet frame handed to a @c packet_handler.
struct packet_view;

/// @brief One-thread DPDK RX endpoint. @see dpdk.hpp
class receiver;

} // namespace exchange::transport::dpdk

#endif // ORDER_BOOK_WITH_DPDK
