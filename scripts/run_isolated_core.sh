#!/usr/bin/env bash
set -e

# run_isolated_core.sh - Wraps execution with taskset for CPU isolation

if [ "$#" -eq 0 ]; then
    echo "Usage: $0 <command> [args...]"
    exit 1
fi

# We pin the process to cores 2 and 3 using taskset
CORES="2,3"

echo "Executing pinned to cores $CORES: $@"
exec taskset -c "$CORES" "$@"
