#!/usr/bin/env bash

set -euo pipefail

if ip link show dev ot_peer >/dev/null 2>&1; then
    sudo ip link delete dev ot_peer
    echo "Removed OptiTrade Engine test pair: ot_peer <-> ot_eng"
elif ip link show dev ot_eng >/dev/null 2>&1; then
    sudo ip link delete dev ot_eng
    echo "Removed OptiTrade Engine test pair: ot_peer <-> ot_eng"
else
    echo "OptiTrade Engine veth pair is not present."
fi
