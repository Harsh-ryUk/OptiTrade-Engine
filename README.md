# OptiTrade Engine

**OptiTrade Engine** is a high-performance, ultra-low-latency C++20 algorithmic trading engine designed for extreme throughput environments using Intel DPDK. 

Bypassing the traditional Linux kernel network stack, OptiTrade Engine processes Ethernet frames entirely in userspace. Combined with zero-copy ring buffers, lock-free queues, and hardware-accelerated memory management, it consistently achieves sub-microsecond tick-to-trade latency.

## Key Features

- **Userspace Networking**: Powered by DPDK for 10Gbps+ line-rate packet processing without kernel interrupts.
- **Multi-Symbol Order Book**: Independent L2 limit order books supporting parallel, cache-friendly matching for multiple instruments.
- **Deterministic Latency**: Zero dynamic allocations in the hot path. Pre-allocated memory pools and ring buffers ensure completely bounded execution times.
- **Pluggable Strategies**: Built-in support for multiple alpha models including VWAP and Momentum strategies, easily toggled at compile or initialization time.
- **Sequence Gap Detection**: Built-in tracking of inbound UDP multicast sequences to immediately flag and drop bad packet runs.
- **Risk Limits Guard**: In-flight order validation preventing fat-finger errors before they reach the wire.
- **Cancel & Replace**: Support for updating pending orders without losing queue priority in the matching engine.

## Architecture

1. **Wire Protocol**: Custom lightweight 80-byte binary protocol for ingress market data and egress orders.
2. **Book Building**: Fixed depth (top 5 levels) L2 order book updated incrementally.
3. **Strategy Layer**: Evaluates imbalance, momentum, and VWAP continuously as the book ticks.
4. **Outbox Management**: Pre-allocated array storing emitted orders for immediate serialization into DPDK Tx bursts.

## Requirements

- **OS**: Linux (Ubuntu 22.04 / 24.04 recommended)
- **Compiler**: GCC 11+ or Clang 14+ (C++20 support required)
- **Dependencies**: 
  - Intel DPDK (v22.11+ recommended)
  - CMake (3.20+)
  - Ninja
- **Hardware**: CPU supporting SSE4.2/AVX2, NIC compatible with DPDK (e.g., Intel 82599, Mellanox ConnectX).

## Build Instructions

1. **Install Dependencies**:
```bash
sudo apt-get update
sudo apt-get install build-essential cmake ninja-build libnuma-dev python3-pyelftools
```

2. **Configure and Build**:
```bash
cmake -S . -B build-dpdk -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DOPTITRADE_ENABLE_DPDK=ON
cmake --build build-dpdk
```

## Running Benchmarks

OptiTrade Engine comes with extensive benchmarking suites for both software routing (`AF_PACKET`) and raw DPDK latency.

To run the software-only latency benchmark (no DPDK hardware required):
```bash
sudo ./build-dpdk/apps/optitrade_dpdk_afpacket_latency
```

To run the full DPDK latency benchmark (requires bound DPDK NIC):
```bash
sudo ./build-dpdk/apps/optitrade_dpdk_latency_benchmark
```

Analyze latency using the provided python script:
```bash
python3 tools/plot_latency.py benchmark_output.txt
```

## Docker Environment

For a quick start without configuring DPDK on your host, you can use the provided Docker setup.

```bash
sudo ./scripts/docker_run.sh
```

*(Note: Docker must have access to 2MB hugepages on the host machine)*

## License
MIT License. See `LICENSE` for more information.
