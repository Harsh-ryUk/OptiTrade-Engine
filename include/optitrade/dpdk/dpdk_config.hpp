#pragma once

#include <cstddef>
#include <cstdint>

namespace optitrade::dpdk {

inline constexpr std::uint32_t kMempoolSize = 4095;
inline constexpr std::uint32_t kMempoolCacheSize = 0;
inline constexpr std::uint16_t kDataRoomSize = 2048;

inline constexpr std::uint32_t kRingSize = 1024;

inline constexpr std::uint16_t kRxQueues = 1;
inline constexpr std::uint16_t kTxQueues = 1;
inline constexpr std::uint16_t kRxDescriptors = 128;
inline constexpr std::uint16_t kTxDescriptors = 128;

inline constexpr std::uint16_t kBurstSize = 8;
inline constexpr std::uint32_t kReceivePollAttempts = 100000;

/*
 * Keep all DPDK object names deliberately short.
 * DPDK internally creates prefixed memzone/ring names.
 */
inline constexpr const char* kMempoolName = "ot_mp";
inline constexpr const char* kMarketDataRingName = "ot_md";
inline constexpr const char* kOutboundOrderRingName = "ot_ord";

inline constexpr const char* kGeneratorPortName = "ot_gen";
inline constexpr const char* kEnginePortName = "ot_eng";

}  // namespace optitrade::dpdk
