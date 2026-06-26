# Upcoming Version Notes

The OptiTrade Engine roadmap focuses on pushing beyond userspace networking and deeper into kernel bypass and hardware-level precision.

## 1. AF_XDP Support

While DPDK provides massive throughput, maintaining bound PCI devices is cumbersome in cloud environments. 
- **Goal**: Implement an alternative `AF_XDP` network backend. `AF_XDP` allows zero-copy packet processing using the Linux kernel's eXpress Data Path (XDP), seamlessly co-existing with traditional network interfaces.
- **Benefit**: Retains userspace speed without the strict NIC hardware ownership requirements of DPDK.

## 2. Hardware Timestamping (PTP/NIC)

The current implementation measures software latency between the moment DPDK surfaces a packet to the application layer and the transmission enqueue.
- **Goal**: Read hardware timestamps directly from the NIC descriptor ring (using `RTE_MBUF_DYNFIELD`).
- **Benefit**: Measures the true wire-to-wire latency, bypassing PCI transfer and interrupt latency artifacts.

## 3. Real Market Data PCAP Replay

Our synthetic benchmarks demonstrate maximum throughput, but they lack the micro-burst characteristics of real market opens.
- **Goal**: Implement a PCAP ingestion tool to replay exact Chicago Mercantile Exchange (CME) or NASDAQ ITCH data captures directly into the `dpdk_latency_benchmark`.
- **Benefit**: Ensures the VWAP and Momentum strategies are validated against real-world volatility and order book crossing scenarios.
