#!/usr/bin/env python3
"""Render the 1280x640 repository social preview card.

    python3 tools/social_preview.py docs/img/social_preview.png
"""
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch

BG, FG, DIM, ACCENT = "#0f172a", "#f8fafc", "#94a3b8", "#38bdf8"

STATS = [
    ("268.7 M", "real Nasdaq messages\nreplayed, zero errors"),
    ("5.2 M/s", "feed replay on the\nfull trading day"),
    ("~80 ns", "engine processing\nper message"),
    ("37 / 8", "test programs /\nCI jobs, all green"),
]


def main(dst):
    fig = plt.figure(figsize=(12.8, 6.4), dpi=100)
    fig.patch.set_facecolor(BG)
    ax = fig.add_axes([0, 0, 1, 1])
    ax.set_xlim(0, 1280)
    ax.set_ylim(0, 640)
    ax.axis("off")

    ax.text(64, 540, "OptiTrade", color=FG, fontsize=54, fontweight="bold", va="center")
    ax.text(64, 470, "Deterministic C++20 trading client", color=ACCENT, fontsize=26, va="center")
    ax.text(64, 425, "Nasdaq ITCH 5.0 market data  >  order books  >  strategy  >  risk  >  OUCH 4.2 orders",
            color=DIM, fontsize=15, va="center")

    x = 64
    for big, small in STATS:
        ax.add_patch(FancyBboxPatch((x, 150), 270, 190, boxstyle="round,pad=0,rounding_size=14",
                                    fc="#1e293b", ec="#334155", lw=1.5))
        ax.text(x + 135, 285, big, color=FG, fontsize=34, fontweight="bold", ha="center", va="center")
        ax.text(x + 135, 205, small, color=DIM, fontsize=13, ha="center", va="center", linespacing=1.4)
        x += 290

    ax.text(64, 78, "bit-identical results on macOS, gcc and clang   |   allocation-free hot path   |   fuzzed decoders",
            color=DIM, fontsize=14, va="center")
    ax.text(64, 40, "github.com/Harsh-ryUk/OptiTrade-Engine", color=ACCENT, fontsize=14, va="center")
    fig.savefig(dst, facecolor=BG)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
