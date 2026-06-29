#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>

#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_pause.h>
#include <rte_timer.h>
#include <rte_version.h>

#include "optitrade/dpdk/engine_wire_adapter.hpp"
#include "optitrade/engine/trading_engine.hpp"
#include "optitrade/wire/ethernet_frame.hpp"

namespace {

inline constexpr std::uint16_t kRxQueues = 1;
inline constexpr std::uint16_t kTxQueues = 1;
inline constexpr std::uint16_t kRxDescriptors = 512;
inline constexpr std::uint16_t kTxDescriptors = 512;
inline constexpr std::uint32_t kMempoolSize = 4095;
inline constexpr std::uint16_t kDataRoomSize = 4096;
inline constexpr std::uint16_t kBurstSize = 8;
inline constexpr std::uint64_t kTimeoutSeconds = 30;
inline constexpr const char* kMempoolName = "ot_amp";

struct Counters {
    std::uint64_t received_packets{};
    std::uint64_t valid_market_packets{};
    std::uint64_t invalid_packets{};
    std::uint64_t no_signal_events{};
    std::uint64_t orders_emitted{};
    std::uint64_t risk_rejected{};
    std::uint64_t outbox_full{};
    std::uint64_t gap_detected{};
};

void print_port_error(const char* operation,
                      const int result) noexcept {
    std::fprintf(
        stderr,
        "%s failed: %s\n",
        operation,
        rte_strerror(result < 0 ? -result : result));
}

optitrade::EngineConfig make_engine_config() noexcept {
    optitrade::EngineConfig config{};

    config.symbol_id = 0; // Unused in multi-symbol mode
    config.strategy.imbalance_threshold_bps = 6000;
    config.strategy.order_quantity = 10;

    config.risk.max_order_quantity = 100;
    config.risk.max_absolute_position = 1000;
    config.risk.max_notional_ticks = 1'000'000'000ULL;
    config.risk.max_outstanding_orders = 64;

    return config;
}

bool configure_port(const std::uint16_t port_id,
                    rte_mempool* const mempool) noexcept {
    rte_eth_conf configuration{};

    int result = rte_eth_dev_configure(
        port_id,
        kRxQueues,
        kTxQueues,
        &configuration);

    if (result < 0) {
        print_port_error("rte_eth_dev_configure", result);
        return false;
    }

    result = rte_eth_rx_queue_setup(
        port_id,
        0,
        kRxDescriptors,
        rte_socket_id(),
        nullptr,
        mempool);

    if (result < 0) {
        print_port_error("rte_eth_rx_queue_setup", result);
        return false;
    }

    result = rte_eth_tx_queue_setup(
        port_id,
        0,
        kTxDescriptors,
        rte_socket_id(),
        nullptr);

    if (result < 0) {
        print_port_error("rte_eth_tx_queue_setup", result);
        return false;
    }

    result = rte_eth_dev_start(port_id);

    if (result < 0) {
        print_port_error("rte_eth_dev_start", result);
        return false;
    }

    return true;
}

optitrade::wire::EthernetEnvelope response_envelope(
    const optitrade::wire::EthernetEnvelope& received) noexcept {
    optitrade::wire::EthernetEnvelope response{};
    response.destination = received.source;
    response.source = received.destination;
    return response;
}

bool send_order_packet(
    const std::uint16_t port_id,
    rte_mempool* const mempool,
    const optitrade::wire::EthernetEnvelope& envelope,
    const optitrade::OrderRequest& order,
    std::uint32_t& outbound_sequence) noexcept {
    optitrade::wire::OutboundOrderMessage wire_order{};

    if (!optitrade::dpdk::EngineWireAdapter::to_wire_order(
            order,
            outbound_sequence++,
            wire_order)) {
        return false;
    }

    optitrade::wire::EthernetFrame frame{};

    if (!optitrade::wire::encode_order_frame(
            envelope,
            wire_order,
            frame)) {
        return false;
    }

    rte_mbuf* const packet = rte_pktmbuf_alloc(mempool);

    if (packet == nullptr) {
        return false;
    }

    void* const payload = rte_pktmbuf_append(
        packet,
        static_cast<std::uint16_t>(frame.size()));

    if (payload == nullptr) {
        rte_pktmbuf_free(packet);
        return false;
    }

    std::memcpy(payload, frame.data(), frame.size());

    rte_mbuf* packets[] = {packet};

    if (rte_eth_tx_burst(port_id, 0, packets, 1) != 1) {
        rte_pktmbuf_free(packet);
        return false;
    }

    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (rte_eal_init(argc, argv) < 0) {
        std::fprintf(stderr, "EAL initialization failed\n");
        return EXIT_FAILURE;
    }

    const std::uint16_t available_ports = rte_eth_dev_count_avail();

    if (available_ports == 0) {
        std::fprintf(
            stderr,
            "No DPDK port found. Start this executable with "
            "--vdev=eth_af_packet0,iface=ot_eng,qpairs=1\n");
        rte_eal_cleanup();
        return EXIT_FAILURE;
    }

    std::uint16_t port_id{};
    bool found_port = false;
    std::uint16_t current_port{};

    RTE_ETH_FOREACH_DEV(current_port) {
        port_id = current_port;
        found_port = true;
        break;
    }

    if (!found_port) {
        std::fprintf(stderr, "Unable to select AF_PACKET DPDK port\n");
        rte_eal_cleanup();
        return EXIT_FAILURE;
    }

    rte_mempool* const mempool = rte_pktmbuf_pool_create(
        kMempoolName,
        kMempoolSize,
        0,
        0,
        kDataRoomSize,
        rte_socket_id());

    if (mempool == nullptr) {
        std::fprintf(
            stderr,
            "Mempool creation failed: %s\n",
            rte_strerror(rte_errno));
        rte_eal_cleanup();
        return EXIT_FAILURE;
    }

    if (!configure_port(port_id, mempool)) {
        rte_mempool_free(mempool);
        rte_eal_cleanup();
        return EXIT_FAILURE;
    }

    optitrade::TradingEngine<64> engine(make_engine_config());

    Counters counters{};
    std::uint32_t outbound_sequence = 5001;
    bool success = true;

    std::printf("============================================================\n");
    std::printf("OptiTrade Engine Level 3 Phase 9B - DPDK AF_PACKET Engine\n");
    std::printf("============================================================\n");
    std::printf("DPDK version:              %s\n", rte_version());
    std::printf("Main lcore:                %u\n", rte_lcore_id());
    std::printf("Available DPDK ports:      %u\n", available_ports);
    std::printf("Selected port:             %u\n", port_id);
    std::printf("Linux interface:           ot_eng\n");
    std::printf("Classification:            Kernel-backed DPDK AF_PACKET\n");
    std::printf("Waiting for packets...\n");
    std::fflush(stdout);

    const std::uint64_t deadline =
        rte_get_timer_cycles() +
        rte_get_timer_hz() * kTimeoutSeconds;

    while (success &&
           counters.orders_emitted == 0 &&
           rte_get_timer_cycles() < deadline) {
        rte_mbuf* packets[kBurstSize]{};

        const std::uint16_t received = rte_eth_rx_burst(
            port_id,
            0,
            packets,
            kBurstSize);

        if (received == 0) {
            rte_pause();
            continue;
        }

        for (std::uint16_t index = 0; index < received; ++index) {
            rte_mbuf* const packet = packets[index];
            ++counters.received_packets;

            const auto* const data =
                rte_pktmbuf_mtod(packet, const std::byte*);

            const auto bytes = std::span<const std::byte>(
                data,
                static_cast<std::size_t>(
                    rte_pktmbuf_data_len(packet)));

            optitrade::wire::EthernetEnvelope received_envelope{};
            optitrade::wire::MarketDataMessage market_message{};

            const auto decode_result =
                optitrade::wire::decode_market_data_frame(
                    bytes,
                    received_envelope,
                    market_message);

            rte_pktmbuf_free(packet);

            if (decode_result !=
                optitrade::wire::FrameDecodeError::none) {
                ++counters.invalid_packets;
                continue;
            }

            ++counters.valid_market_packets;

            optitrade::MarketUpdate update{};

            if (!optitrade::dpdk::EngineWireAdapter::to_market_update(
                    market_message,
                    update)) {
                ++counters.invalid_packets;
                continue;
            }

            const optitrade::EngineResult result =
                engine.on_market_update(update);

            switch (result.status) {
                case optitrade::EngineStatus::no_order:
                    ++counters.no_signal_events;
                    break;

                case optitrade::EngineStatus::gap_detected:
                    ++counters.gap_detected;
                    break;

                case optitrade::EngineStatus::risk_rejected:
                    ++counters.risk_rejected;
                    break;

                case optitrade::EngineStatus::outbox_full:
                    ++counters.outbox_full;
                    break;

                case optitrade::EngineStatus::invalid_update:
                    ++counters.invalid_packets;
                    break;

                case optitrade::EngineStatus::order_emitted: {
                    optitrade::OrderRequest order{};

                    if (!engine.pop_order(order) ||
                        !send_order_packet(
                            port_id,
                            mempool,
                            response_envelope(received_envelope),
                            order,
                            outbound_sequence)) {
                        success = false;
                        break;
                    }

                    ++counters.orders_emitted;
                    break;
                }
            }

            if (!success || counters.orders_emitted != 0) {
                break;
            }
        }
    }

    if (counters.orders_emitted != 1) {
        std::fprintf(stderr, "No outbound order was generated\n");
        success = false;
    }

    rte_eth_stats stats{};
    rte_eth_stats_get(port_id, &stats);

    if (success) {
        std::printf("\nAF_PACKET engine result:\n");
        std::printf("  Received packets:         %llu\n",
                    static_cast<unsigned long long>(
                        counters.received_packets));
        std::printf("  Valid market packets:     %llu\n",
                    static_cast<unsigned long long>(
                        counters.valid_market_packets));
        std::printf("  No-signal events:         %llu\n",
                    static_cast<unsigned long long>(
                        counters.no_signal_events));
        std::printf("  Orders emitted:           %llu\n",
                    static_cast<unsigned long long>(
                        counters.orders_emitted));
        std::printf("  Invalid packets:          %llu\n",
                    static_cast<unsigned long long>(
                        counters.invalid_packets));
        std::printf("  Gap detected:             %llu\n",
                    static_cast<unsigned long long>(
                        counters.gap_detected));
        std::printf("  Risk rejected:            %llu\n",
                    static_cast<unsigned long long>(
                        counters.risk_rejected));
        std::printf("  PMD RX packets:           %llu\n",
                    static_cast<unsigned long long>(stats.ipackets));
        std::printf("  PMD TX packets:           %llu\n",
                    static_cast<unsigned long long>(stats.opackets));
        std::printf("\n");
        std::printf("This is a kernel-backed AF_PACKET functional path.\n");
        std::printf("It is not VFIO physical-NIC bypass latency.\n");
        std::printf("\nStatus:                    OK\n");
    }

    const int stop_result = rte_eth_dev_stop(port_id);
    if (stop_result < 0) {
        print_port_error("rte_eth_dev_stop", stop_result);
    }

    const int close_result = rte_eth_dev_close(port_id);
    if (close_result < 0) {
        print_port_error("rte_eth_dev_close", close_result);
    }

    rte_mempool_free(mempool);

    const int cleanup_result = rte_eal_cleanup();

    if (!success || cleanup_result != 0) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
