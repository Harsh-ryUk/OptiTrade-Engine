#!/usr/bin/env bash
set -e

# setup_hugepages.sh - Configures 2MB hugepages for DPDK

HUGEPAGES=1024
MOUNT_POINT="/mnt/huge"

echo "Setting up $HUGEPAGES 2MB hugepages..."

# Allocate hugepages
echo $HUGEPAGES | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages > /dev/null

# Create mount point if it doesn't exist
if [ ! -d "$MOUNT_POINT" ]; then
    sudo mkdir -p "$MOUNT_POINT"
fi

# Mount hugetlbfs if not already mounted
if ! mount | grep -q "$MOUNT_POINT"; then
    sudo mount -t hugetlbfs nodev "$MOUNT_POINT"
fi

echo "Hugepages setup complete. Current hugepages:"
cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
