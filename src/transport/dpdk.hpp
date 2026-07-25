#pragma once

#ifdef ORDER_BOOK_WITH_DPDK

#include "transport_export.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>

namespace transport::dpdk {

/** Configuration for one NIC receive queue and its NUMA-local mbuf pool. */
struct receiver_config {
	std::uint16_t port_id{0};
	std::uint16_t rx_queue_id{0};
	std::uint16_t rx_descriptors{1024};
	std::uint32_t mbuf_count{8192};
	std::uint32_t mbuf_cache_size{256};
	std::string mempool_name{"order_book_dpdk_rx"};
};

/** A borrowed DPDK Ethernet frame. Its bytes expire when the callback returns.
 */
struct packet_view {
	std::span<const std::byte> bytes;
	std::uint64_t receive_tsc;
};

/// Called synchronously from receiver::poll(). It must not retain packet bytes.
using packet_handler = void (*)(packet_view packet, void *context) noexcept;

/**
 * @brief One-thread DPDK RX endpoint.
 *
 * The instance owns EAL initialisation, one RX queue, and its mbuf pool. One
 * thread must call poll(); use an owner-directed bounded queue to transfer
 * decoded commands into an engine partition. This class intentionally exposes
 * raw frames only, keeping protocol parsing and order-book mutation outside
 * the transport layer.
 */
class TRANSPORT_EXPORT receiver {
public:
	explicit receiver(receiver_config config = {});
	~receiver();

	receiver(const receiver &)            = delete;
	receiver &operator=(const receiver &) = delete;
	receiver(receiver &&)                 = delete;
	receiver &operator=(receiver &&)      = delete;

	/// Initialise DPDK EAL, the mbuf pool, and the configured RX queue.
	/// @p argv must contain DPDK EAL arguments and may be modified by DPDK.
	[[nodiscard]] std::expected<void, std::string> initialize(int argc,
															  char **argv);

	/// Poll at most kMaxBurst frames and synchronously dispatch contiguous
	/// ones. Non-contiguous frames are dropped and counted; every received mbuf
	/// is released before this call returns.
	[[nodiscard]] std::uint16_t poll(packet_handler handler,
									 void *context) noexcept;

	/// Stop the port, release the mempool, and clean up the EAL instance.
	void shutdown() noexcept;

	[[nodiscard]] bool is_initialized() const noexcept;
	[[nodiscard]] std::uint64_t dropped_noncontiguous() const noexcept;

	static constexpr std::uint16_t kMaxBurst = 32;

private:
	struct state;
	receiver_config config_;
	std::unique_ptr<state> state_;
};

} // namespace transport::dpdk

#endif // ORDER_BOOK_WITH_DPDK
