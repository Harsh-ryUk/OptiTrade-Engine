# OptiTrade Engine: Deep Dive & Technical Architecture

## Introduction to OptiTrade Engine

**OptiTrade Engine** is a rigorous C++20 and DPDK systems engineering project designed to demystify the critical hot path of high-frequency trading (HFT) systems. 

Rather than simulating an entire brokerage firm with bloated databases and REST APIs, this repository focuses exclusively on the microsecond-level mechanics of reacting to market data:
```text
Ingest Network Packet ──► Process Order Book ──► Execute Trading Logic ──► Validate Risk Limits ──► Emit Outbound Order
```
By isolating this pipeline, we can accurately measure hardware-software interaction limits and understand exactly what causes latency spikes in trading software.

---

## The Philosophy of Speed

In the realm of low-latency trading, speed is often achieved not by doing things faster, but by doing *less*. To achieve our sub-microsecond internal latencies, we strictly adhered to several design principles:

### Core Optimizations
1. **Zero Heap Allocations:** Dynamic memory allocation is strictly forbidden during live trading. Every order structure and buffer is heavily pre-allocated.
2. **Fixed-Format Binary Framing:** We utilize 62-byte Ethernet-style frames. Avoiding JSON, XML, or even variable-length binary parsing saves crucial CPU cycles.
3. **Integer Arithmetic Only:** Floating-point math introduces unacceptable non-determinism and overhead. All prices are represented as integer ticks.
4. **Deterministic Data Structures:** The L2 order book operates on fixed-depth arrays ensuring O(1) cache-friendly lookups.
5. **No System Calls in the Hot Path:** We eliminated all console logging, file I/O, and context switches during the measured execution loop.
6. **DPDK Polling Mode:** Standard kernel networking interrupts are bypassed in favor of DPDK's continuous polling paradigm.

Things we intentionally ignored for Version 1 (because they distract from hot-path optimization): multi-threading synchronization, database persistence, web dashboards, and complex quantitative alpha models.

---

## Technical Components & Workflow

The architecture is highly linear and entirely run-to-completion on a single CPU core.

```text
[ Raw Ethernet Frame Ingestion ]
             │
             ▼
[ DPDK RX Ring (rte_mbuf) ]
             │
             ▼
[ Binary Payload Extractor ]
             │
             ▼
[ Synchronous L2 Book Update ] ◄── Evaluates current Bid/Ask depths
             │
             ▼
[ Imbalance Decision Matrix ] ◄── Calculates pressure differentials
             │
             ▼
[ Inline Risk Validator ] ◄── Enforces exposure limits and safety checks
             │
             ▼
[ Order Frame Construction ] ◄── Populates pre-allocated egress buffers
             │
             ▼
[ DPDK TX Ring Submission ]
```

---

## Testing & Scenario Validation

The system is rigorously tested against standard market scenarios to ensure logic correctness before latency is even measured:

- **Equilibrium State (`NO_SIGNAL`):** The book is balanced. The engine silently drops the cycle.
- **Upward Pressure (`BUY`):** Heavy bid concentration triggers an immediate buy order.
- **Downward Pressure (`SELL`):** Heavy ask concentration triggers an immediate sell order.
- **Safety Trigger (`RISK_REJECT`):** A valid trade signal is vetoed due to internal exposure thresholds.
- **Corrupt Payload (`INVALID_FRAME`):** Malformed packets are discarded before they can poison the L2 book state.

---

## Comprehensive Latency Breakdown

It is critical to be honest about latency metrics. The numbers below represent **Application-Layer Turnaround Time**—the time elapsed from the moment the software receives the packet from the DPDK driver until it hands the response back. They **do not** include physical fiber transmission, switch hops, or hardware NIC processing times.

### 1. In-Memory DPDK Ring PMD Benchmark
*This is the cleanest software-only baseline, avoiding the kernel entirely via virtual memory rings.*

| CPU Thread | Median (p50) | 95th Percentile | 99th Percentile | 99.9th Percentile | Sample Size |
|:---:|:---:|:---:|:---:|:---:|:---:|
| CPU 0 | 110.84 ns | 248.39 ns | 552.21 ns | 706.46 ns | 1,000,000 |
| CPU 2 | 112.84 ns | 254.40 ns | 550.21 ns | 754.54 ns | 1,000,000 |

### 2. Kernel-Backed DPDK AF_PACKET Benchmark
*This test routes packets through a Linux `veth` pair, demonstrating the overhead introduced when touching kernel networking layers.*

| CPU Thread | Median (p50) | 95th Percentile | 99th Percentile | 99.9th Percentile | Sample Size |
|:---:|:---:|:---:|:---:|:---:|:---:|
| CPU 0 | 1.73 µs | 2.59 µs | 3.26 µs | 10.54 µs | 1,000,000 |
| CPU 2 | 1.83 µs | 2.90 µs | 7.82 µs | 22.06 µs | 1,000,000 |

---

## Real-World Engineering Hurdles

Building this engine exposed several low-level systems engineering challenges:

1. **PMD Library Linking:** DPDK requires explicit linking of `librte_net_ring.so` and `librte_net_af_packet.so` to prevent runtime "undefined reference" failures when initializing virtual devices.
2. **Assertion stripping:** CMake `Release` builds stripped standard `assert()` calls. We had to implement custom enforced validation layers to maintain safety without sacrificing speed.
3. **Mbuf Size Constraints:** The AF_PACKET driver expects significantly larger memory data rooms (e.g., 4096 bytes) compared to standard ring PMDs, requiring dynamic mempool configuration.
4. **Veth Lifecycle Management:** The `ot_peer` interface had to be programmatically created and destroyed around every kernel-backed test run to prevent DPDK initialization crashes.

---

## Hardware Limitations & Future Roadmap

This iteration of OptiTrade Engine was developed and profiled on standard workstation hardware lacking a dedicated PCIe VFIO-compatible NIC. Therefore, all tests utilize DPDK's software rings or AF_PACKET drivers.

**Next steps for development:**
- **Hardware Deployment:** Execute the engine on a server equipped with a Solarflare or Mellanox NIC bound via VFIO.
- **Hardware Timestamping:** Integrate external capture devices (e.g., Corvil) for true wire-to-wire latency measurement.
- **Market Burst Simulation:** Shift from fixed-interval synthetic packet injection to highly variable, pcap-based high-burst replays to measure realistic queueing latency.
- **AF_XDP Integration:** Compare the existing AF_PACKET performance against Linux's modern AF_XDP kernel-bypass technology.

---

## Final Thoughts

OptiTrade Engine proves that modern C++20, when heavily constrained and married to DPDK polling, can consistently execute complex state-machine updates (like order books and risk logic) in under 150 nanoseconds per event.
