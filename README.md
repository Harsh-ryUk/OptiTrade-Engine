# OptiTrade Engine: A C++20 + DPDK Low-Latency Trading Packet Processor

![C++](https://img.shields.io/badge/C++-20-blue.svg) ![DPDK](https://img.shields.io/badge/DPDK-25.11-lightgrey.svg) ![License](https://img.shields.io/badge/License-MIT-green.svg) [![Build Status](https://github.com/Harsh-ryUk/OptiTrade-Engine/actions/workflows/build.yml/badge.svg)](https://github.com/Harsh-ryUk/OptiTrade-Engine/actions)

**Maintained by:** Harsh

## Overview
**OptiTrade Engine** is a high-performance, ultra-low-latency packet processor built in C++20 and powered by the Data Plane Development Kit (DPDK). It is explicitly engineered to handle fixed-size Ethernet market-data packets, maintain an extremely fast in-memory L2 order book, execute a streamlined order-imbalance algorithm, evaluate pre-trade risk, and dispatch outbound order frames seamlessly through DPDK RX/TX pipelines.

This repository serves as a proof-of-concept for **critical path optimization in algorithmic trading**:
```text
Ingress Market Packet ──► Decode ──► L2 Book Sync ──► Imbalance Strategy ──► Risk Guard ──► Order Encode ──► Egress TX Enqueue
```
*Note: The metrics presented reflect internal application engine latency, not physical wire-to-wire or exchange latencies.*

---

## Core Objectives
Our primary goal was to strip away the bloat and determine the absolute minimum overhead required to process a trade signal. We deliberately excluded heavy backend architectures—like REST interfaces, database calls, or complex message queues—to focus strictly on the execution hot path.

Key architectural tenets:
- **Fixed-format Binary Messaging:** Avoids parsing overhead.
- **Integer Ticks:** Eliminates floating-point arithmetic delays.
- **Pre-allocated Memory:** Zero dynamic heap allocation (`new`/`malloc`) during live processing.
- **Silent Hot Path:** Absolutely no string formatting or logging during critical execution.
- **DPDK Polling:** Maximizes throughput and minimizes interrupt latency.

---

## Current Development Status

| Component / Feature | Completion Status |
|---|---|
| Core C++20 Execution Engine | Fully Integrated |
| Custom Binary Wire Protocol | Fully Integrated |
| 62-byte Ethernet Frame Processing | Fully Integrated |
| Deterministic L2 Order Book | Fully Integrated |
| Signal Generation (BUY/SELL/HOLD) | Fully Integrated |
| Zero-allocation Risk Checks | Fully Integrated |
| DPDK Ring PMD Virtual Environment | Fully Integrated |
| DPDK AF_PACKET via Linux `veth` | Fully Integrated |
| Physical NIC / VFIO Hardware Testing | Pending |
| High-Burst Market Replay Simulation | Planned |

---

## Technical Architecture

```text
[ Ethernet Market Data Frame ]
             │
             ▼
[ DPDK RX Polling / rte_mbuf ]
             │
             ▼
[ Binary Protocol Decoder ]
             │
             ▼
[ Market Update Object ]
             │
             ▼
     [ Trading Engine ]
     ├── L2 Order Book
     ├── Imbalance Strategy
     └── Risk Guard
             │
             ▼
[ Preallocated Order Buffer ]
             │
             ▼
[ Ethernet Order Frame Encoder ]
             │
             ▼
[ DPDK TX Transmission Queue ]
```

---

## Strategy Implementation
For Version 1, the trading algorithm is deliberately straightforward. The system analyzes the visible liquidity within the L2 book and generates signals based on pressure:
- **Dominant Bids** ──► Trigger `BUY`
- **Dominant Asks** ──► Trigger `SELL`
- **Equilibrium** ──► Trigger `NO_SIGNAL`
- **Limit Breach** ──► Trigger `RISK_REJECT`
- **Malformed Data** ──► Trigger `INVALID_FRAME`

The objective isn't alpha generation, but rather providing a predictable decision tree to stress-test the pipeline.

---

## Latency Benchmarks
*Disclaimer: These microbenchmarks evaluate the application-layer processing bounds using synthetic fixed-size packets. They do not account for physical network traversal or hardware switch delays.*

### 1. DPDK Ring PMD (Virtual Path)
Measurement scope: From `rte_eth_rx_burst()` packet retrieval to `rte_eth_tx_burst()` order submission.

| CPU Core | p50 (ns) | p95 (ns) | p99 (ns) | p99.9 (ns) | Total Events | Dropped |
|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| Core 0 | 110.84 | 248.39 | 552.21 | 706.46 | 1,000,000 | 0 |
| Core 2 | 112.84 | 254.40 | 550.21 | 754.54 | 1,000,000 | 0 |

### 2. DPDK AF_PACKET (Kernel-Backed via `veth`)
Measurement scope: RX-to-TX turnaround over a private virtual ethernet pair.

| CPU Core | p50 (µs) | p95 (µs) | p99 (µs) | p99.9 (µs) | Total Events | Dropped |
|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| Core 0 | 1.73 | 2.59 | 3.26 | 10.54 | 1,000,000 | 0 |
| Core 2 | 1.83 | 2.90 | 7.82 | 22.06 | 1,000,000 | 0 |

---

## System Requirements
- OS: Ubuntu 26.04
- Compiler: GCC 15.2.0 (C++20 Support)
- Build Tools: CMake, Ninja
- Dependencies: DPDK 25.11.0

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build pkg-config dpdk dpdk-dev libdpdk-dev
```

---

## Build Instructions

```bash
cmake -S . -B build-dpdk -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DOPTITRADE_ENABLE_DPDK=ON -DOPTITRADE_BUILD_BENCHMARKS=ON
cmake --build build-dpdk -j"$(nproc)"
```

Execute unit tests:
```bash
ctest --test-dir build-dpdk --output-on-failure
```

---

## Execution Guides

### Validating Ring PMD Scenarios
```bash
./build-dpdk/optitrade_dpdk_scenarios -l 0 --no-pci --no-huge --in-memory --mbuf-pool-ops-name=ring_mp_mc --file-prefix=ot_scenarios
```

### Running Ring PMD Latency Tests
```bash
./build-dpdk/optitrade_dpdk_latency_benchmark -l 0 --no-pci --no-huge --in-memory --mbuf-pool-ops-name=ring_mp_mc --file-prefix=ot_ring_c0
```

### Running AF_PACKET Tests
Initialize interface:
```bash
./scripts/level3/setup_veth_afpacket.sh
```

Launch Engine:
```bash
AF_PACKET_LIB="$(find /usr/lib/x86_64-linux-gnu -name 'librte_net_af_packet.so' -print 2>/dev/null | head -n 1)"
sudo ./build-dpdk/optitrade_dpdk_afpacket_engine -l 0 --no-pci --no-huge --in-memory --mbuf-pool-ops-name=ring_mp_mc -d "${AF_PACKET_LIB}" --vdev='eth_af_packet0,iface=ot_eng,qpairs=1,qdisc_bypass=1' --file-prefix=ot_af_func
```

Send Traffic (New Terminal):
```bash
sudo ./build-dpdk/optitrade_afpacket_generator ot_peer
```

Clean up:
```bash
./scripts/level3/remove_veth_afpacket.sh
```

---

## Conclusion
OptiTrade Engine demonstrates how minimizing software overhead, leveraging fixed structures, and utilizing DPDK can result in nanosecond-scale processing latency for trading signals.
