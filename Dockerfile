FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    pkg-config \
    dpdk \
    dpdk-dev \
    libdpdk-dev

WORKDIR /app
COPY . .

# Build the project
RUN cmake -S . -B build-dpdk -G Ninja -DCMAKE_BUILD_TYPE=Release -DOPTITRADE_ENABLE_DPDK=ON -DOPTITRADE_BUILD_BENCHMARKS=ON
RUN cmake --build build-dpdk -j"$(nproc)"

# Create the runtime image
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    dpdk \
    numactl \
    iproute2 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /app/build-dpdk/optitrade_dpdk_afpacket_engine /usr/local/bin/
COPY --from=builder /app/build-dpdk/optitrade_afpacket_generator /usr/local/bin/
COPY --from=builder /app/scripts/level3/setup_veth_afpacket.sh /app/setup.sh

RUN chmod +x /app/setup.sh

# The default command runs the AF_PACKET engine. 
# Note: For AF_PACKET we need to specify the correct .so path which might vary by Ubuntu version.
CMD ["/usr/local/bin/optitrade_dpdk_afpacket_engine", "-l", "0", "--no-pci", "--no-huge", "--in-memory", "--mbuf-pool-ops-name=ring_mp_mc", "-d", "librte_net_af_packet.so", "--vdev=eth_af_packet0,iface=ot_eng,qpairs=1,qdisc_bypass=1", "--file-prefix=ot_af_func"]
