#!/usr/bin/env bash
set -e

echo "OptiTrade Engine - Docker Environment Setup"

# Check if running as root
if [ "$EUID" -ne 0 ]; then
  echo "Please run as root (or use sudo) to allocate hugepages."
  exit 1
fi

echo "Allocating hugepages..."
echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

echo "Starting Docker Compose..."
docker-compose up --build
