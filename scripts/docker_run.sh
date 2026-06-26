#!/usr/bin/env bash
# Author: Harsh
set -e

echo "Setting up veth pair for raw AF_PACKET communication (host side, if needed)..."
sudo ./scripts/level3/setup_veth_afpacket.sh || true

echo "Allocating hugepages..."
sudo bash -c 'echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages' || true

echo "Building containers..."
sudo docker-compose build

echo "Starting ot-engine..."
sudo docker-compose up -d ot-engine

echo "Waiting for engine to initialize..."
sleep 3

echo "Starting ot-generator..."
sudo docker-compose up ot-generator
