#include "dpdk.hpp"

#ifdef ORDER_BOOK_WITH_DPDK

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

#include <fmt/format.h>

#include <array>
#include <cerrno>
#include <utility>

namespace exchange::transport::dpdk {

struct receiver::state {
	rte_mempool *mbuf_pool{nullptr};
	bool eal_initialised{false};
	bool port_started{false};
	std::uint64_t dropped_noncontiguous{0};
};

namespace {

[[nodiscard]] std::string dpdk_error(const char *operation) {
	return fmt::format("{}: {}", operation, rte_strerror(rte_errno));
}

[[nodiscard]] std::string dpdk_error(const char *operation, int error) {
	return fmt::format("{}: {}", operation, rte_strerror(-error));
}

} // namespace

receiver::receiver(receiver_config config) : config_(std::move(config)) {}

receiver::~receiver() { shutdown(); }

std::expected<void, std::string> receiver::initialise(int argc, char **argv) {
	if (state_ != nullptr) return std::unexpected("DPDK receiver already initialised");
	if (argc <= 0 || argv == nullptr)
		return std::unexpected("DPDK EAL requires a non-empty argv");

	const int eal_result = rte_eal_init(argc, argv);
	if (eal_result < 0) return std::unexpected(dpdk_error("rte_eal_init"));

	state_ = std::make_unique<state>();
	state_->eal_initialised = true;
	if (!rte_eth_dev_is_valid_port(config_.port_id)) {
		shutdown();
		return std::unexpected("configured DPDK port is not available");
	}

	const int socket_id = rte_eth_dev_socket_id(config_.port_id);
	const unsigned pool_socket = socket_id < 0 ? SOCKET_ID_ANY :
		static_cast<unsigned>(socket_id);
	state_->mbuf_pool = rte_pktmbuf_pool_create(
		config_.mempool_name.c_str(), config_.mbuf_count, config_.mbuf_cache_size,
		0, RTE_MBUF_DEFAULT_BUF_SIZE, pool_socket);
	if (state_->mbuf_pool == nullptr) {
		const std::string error = dpdk_error("rte_pktmbuf_pool_create");
		shutdown();
		return std::unexpected(error);
	}

	rte_eth_conf port_config{};
	int result = rte_eth_dev_configure(config_.port_id, 1, 0, &port_config);
	if (result < 0) {
		const std::string error = dpdk_error("rte_eth_dev_configure", result);
		shutdown();
		return std::unexpected(error);
	}

	std::uint16_t rx_descriptors = config_.rx_descriptors;
	std::uint16_t tx_descriptors = 0;
	result = rte_eth_dev_adjust_nb_rx_tx_desc(config_.port_id, &rx_descriptors,
												  &tx_descriptors);
	if (result < 0) {
		const std::string error =
			dpdk_error("rte_eth_dev_adjust_nb_rx_tx_desc", result);
		shutdown();
		return std::unexpected(error);
	}

	result = rte_eth_rx_queue_setup(config_.port_id, config_.rx_queue_id,
									rx_descriptors, socket_id, nullptr, state_->mbuf_pool);
	if (result < 0) {
		const std::string error = dpdk_error("rte_eth_rx_queue_setup", result);
		shutdown();
		return std::unexpected(error);
	}

	result = rte_eth_dev_start(config_.port_id);
	if (result < 0) {
		const std::string error = dpdk_error("rte_eth_dev_start", result);
		shutdown();
		return std::unexpected(error);
	}
	state_->port_started = true;
	return {};
}

std::uint16_t receiver::poll(packet_handler handler, void *context) noexcept {
	if (state_ == nullptr || !state_->port_started || handler == nullptr) return 0;

	std::array<rte_mbuf *, kMaxBurst> mbufs{};
	const std::uint16_t received = rte_eth_rx_burst(
		config_.port_id, config_.rx_queue_id, mbufs.data(), kMaxBurst);
	for (std::uint16_t index = 0; index < received; ++index) {
		rte_mbuf *mbuf = mbufs[index];
		if (rte_pktmbuf_is_contiguous(mbuf)) {
			const auto *data = static_cast<const std::byte *>(rte_pktmbuf_mtod(mbuf,
				const void *));
			handler(packet_view{std::span{data, rte_pktmbuf_pkt_len(mbuf)},
							rte_rdtsc()}, context);
		} else {
			++state_->dropped_noncontiguous;
		}
		rte_pktmbuf_free(mbuf);
	}
	return received;
}

void receiver::shutdown() noexcept {
	if (state_ == nullptr) return;
	if (state_->port_started) rte_eth_dev_stop(config_.port_id);
	if (state_->eal_initialised) rte_eth_dev_close(config_.port_id);
	if (state_->mbuf_pool != nullptr) rte_mempool_free(state_->mbuf_pool);
	if (state_->eal_initialised) rte_eal_cleanup();
	state_.reset();
}

bool receiver::is_initialised() const noexcept {
	return state_ != nullptr && state_->port_started;
}

std::uint64_t receiver::dropped_noncontiguous() const noexcept {
	return state_ == nullptr ? 0 : state_->dropped_noncontiguous;
}

} // namespace exchange::transport::dpdk

#endif