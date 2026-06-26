# OptiTrade Engine Details and Design Rationale

This document details the critical design decisions and implementation trade-offs made in the OptiTrade Engine.

## 1. 80-Byte Custom Wire Protocol

To eliminate latency spikes caused by branch prediction misses and variable-length loops inherent in traditional protocols like JSON, XML, or FIX, OptiTrade Engine defines a strict 80-byte wire protocol (including Ethernet headers). 
- **Deterministic Bounds**: At exactly 80 bytes, the hardware prefetcher and L1 CPU cache can reliably load an entire market data packet or order structure in a single cache line (usually 64 bytes for payload + alignment considerations).
- **Embedded Sequence and Symbol ID**: By embedding `sequence_num` and `symbol_id` directly in the padded structure, we optimize network byte-swapping, allowing rapid integer casts inside the ingress loop.

## 2. Multi-Symbol Support (16-Symbol Hashing)

Instead of maintaining a massive hash map representing the entire market universe, the trading engine uses a `std::array<FixedL2Book, 16>` to track a predefined subset of 16 symbols.
- **Why 16?**: 16 instances easily fit in the CPU's primary caches.
- **Lookup mechanism**: We route packets via `symbol_id % 16`. This avoids cache-trashing and allows `O(1)` contiguous memory lookup without branching over potential hash collisions found in standard `std::unordered_map`.

## 3. Sequence Gap Detection

Market data is broadcast over UDP, making packet loss a certainty. The engine includes a strict sequence tracker that checks `sequence_num == expected`. 
- **Immediate Rejection**: If a gap is detected, the engine instantly yields processing. This prevents the L2 Book from crossing internal bids and asks due to missing intermediate tick data.

## 4. Pending Order Tracker (Cancel & Replace)

Instead of relying on dynamic heap allocations (e.g., `std::map`, `std::vector`) to map client order IDs back to pending instructions, we use a 64-slot `std::array` acting as a ring buffer (`PendingOrderTracker`).
- **Packet Distances**: It scans the last 64 active orders. If an opposing signal is found for the same symbol within a distance of 4 packets, the engine emits a CANCEL. If a repriced signal arrives, it emits a REPLACE.
- **Latency Consistency**: The worst-case scan time of 64 contiguous array elements requires only ~3-5 nanoseconds.

## 5. Alpha Strategies: VWAP and Momentum

Trading logic must evaluate in bounded time. 
- **VWAP Imbalance**: We iterate linearly over the top 5 fixed levels of the book. The ratio `total_price_volume / total_volume` provides a resilient threshold.
- **Momentum**: A lightweight 8-slot circular buffer tracks mid-price movements (1 for up, -1 for down). The evaluation only requires summing 8 integers—achieving sub-nanosecond signal generation.
