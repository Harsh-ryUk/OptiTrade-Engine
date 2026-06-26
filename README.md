# OptiTrade Engine

**OptiTrade Engine** is a high-performance, ultra-low-latency C++20 algorithmic trading engine designed for extreme throughput environments using Intel DPDK. By bypassing the traditional Linux kernel network stack, OptiTrade processes Ethernet frames entirely in userspace. 

With its zero-copy ring buffers, deterministic lock-free queues, and no dynamic memory allocation on the hot path, it reliably achieves sub-microsecond tick-to-trade latency.

## Architecture

```text
    [Market Data Source]       [Client / Simulator]
          |   (UDP/ETH)             |  (UDP/ETH)
          v                         v
  +-----------------------------------------+
  |               Intel DPDK                |
  |     (Raw userspace packet polling)      |
  +-----------------------------------------+
                    | ingress packets
                    v
  +-----------------------------------------+
  |              Trading Engine             |
  |  +-----------------------------------+  |
  |  |       Sequence Gap Tracker        |  |
  |  +-----------------------------------+  |
  |                    |                    |
  |  +-----------------------------------+  |
  |  | Multi-Symbol L2 Books (16 Arrays) |  |
  |  +-----------------------------------+  |
  |                    |                    |
  |  +-----------------------------------+  |
  |  | Pluggable Alpha Strategy          |  |
  |  |   - VWAP Imbalance                |  |
  |  |   - Momentum                      |  |
  |  +-----------------------------------+  |
  |                    | signal             |
  |  +-----------------------------------+  |
  |  | Risk Guards & Limits Validation   |  |
  |  +-----------------------------------+  |
  |                    |                    |
  |  +-----------------------------------+  |
  |  | Cancel / Replace Order Tracker    |  |
  |  +-----------------------------------+  |
  |                    |                    |
  |  +-----------------------------------+  |
  |  |       Preallocated Outbox         |  |
  |  +-----------------------------------+  |
  +-----------------------------------------+
                    | egress orders
                    v
           [Exchange / Simulator]
```

## Features

- **Multi-Symbol Order Book**: Operates an array of 16 independent L2 limit order books, hashed by `symbol_id % 16`. 
- **Sequence Gap Detection**: Built-in UDP multicast sequence tracker that instantly flags and drops bad packet runs to ensure data integrity.
- **Cancel & Replace Handlers**: A lock-free 64-slot ring buffer tracks pending outbound orders. Generates immediate CANCEL or REPLACE logic if market conditions flip before exchange acknowledgment.
- **Pluggable Strategies**: Switch models at compile-time with `-DOPTITRADE_STRATEGY=momentum` or `vwap_imbalance`.
  - **VWAP Imbalance**: Evaluates total VWAP of bids vs. asks across all L2 levels. Triggers when one side exceeds the other by 0.1%.
  - **Momentum**: Tracks the last 8 mid-price ticks in a circular buffer. Fires if 6 of the last 8 are strongly directional.

## Wire Protocol Specification

A custom, deterministic **80-byte** wire protocol is used (14 bytes Ethernet + 16 bytes custom header + 50 bytes payload). 

| Offset | Type          | Field Name                |
| ------ | ------------- | ------------------------- |
| 0      | `MAC[6]`      | Ethernet Destination MAC  |
| 6      | `MAC[6]`      | Ethernet Source MAC       |
| 12     | `uint16_t`    | EtherType (0x88B5)        |
| **14** | **uint16_t**  | **Header Magic (0x5042)** |
| 16     | `uint8_t`     | Header Version (1)        |
| 17     | `uint8_t`     | Message Type (1/2)        |
| 18     | `uint32_t`    | Sequence Number           |
| 22     | `uint64_t`    | Header Flags/Padding      |
| **30** | **uint64_t**  | **Timestamp NS (Market)** |
| 38     | `uint32_t`    | Symbol ID                 |
| 42     | `int64_t`     | Price Ticks               |
| 50     | `uint32_t`    | Quantity                  |
| 54     | `uint8_t`     | Side (Buy/Sell)           |
| 55     | `uint8_t`     | Action / Order Type       |
| 56     | `uint16_t`    | Level / TIF               |
| 58     | `uint32_t`    | Source Flags              |
| 62     | `uint32_t`    | Sequence Num Tracker      |
| 66     | `padding`     | Alignment padding         |
| **80** |               | **End of Frame**          |

## Build Instructions

1. **Install Dependencies**:
```bash
sudo apt-get update
sudo apt-get install build-essential cmake ninja-build libnuma-dev python3-pyelftools
```

2. **Configure and Build**:
```bash
cmake -S . -B build-dpdk -G Ninja -DCMAKE_BUILD_TYPE=Release -DOPTITRADE_ENABLE_DPDK=ON -DOPTITRADE_STRATEGY=vwap_imbalance
cmake --build build-dpdk -j$(nproc)
```

## Running the Engine via Docker

For rapid testing without native DPDK bindings or hardware, use the Docker container environment. This sets up virtual Ethernet links (veth) and runs the framework using `--in-memory`.

```bash
sudo ./scripts/docker_run.sh
```

## Tools

Generate visualization charts for your latency measurements using the included Python analysis tool:

```bash
python3 tools/plot_latency.py benchmark_output.txt
```
This parses `p50`, `p95`, `p99`, and `p99.9` latency percentiles, outputs a summary table, and saves a bar chart to `results/latency_chart.png`.

## Latency Results

| Metric | Target Value |
|--------|--------------|
| p50    | < 2.0 us     |
| p95    | < 3.5 us     |
| p99    | < 5.0 us     |
| p99.9  | < 12.0 us    |

## Limitations

- Currently limited to 16 active trading symbols dynamically hashed. 
- Docker virtual networking introduces latency artifacts compared to physical DPDK polling.
- Hardware timestamping relies on compatible Intel/Mellanox NICs in bare-metal configurations.

## License

MIT License. See `LICENSE` for more information.
