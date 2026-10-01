#!/usr/bin/env python3
"""Draw an order book depth chart from `ot_book_dump` CSV output.

    ./build/release/ot_book_dump --messages 30000 --depth 12 > book.csv
    python3 tools/plot_book.py book.csv docs/img/book_depth.png
"""
import csv
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


def main(src, dst):
    bids, asks = [], []
    with open(src) as f:
        for row in csv.DictReader(f):
            level = (float(row["price"]), int(row["quantity"]))
            (bids if row["side"] == "bid" else asks).append(level)
    if not bids or not asks:
        sys.exit("need both sides of the book")

    bids.sort(key=lambda l: -l[0])  # best (highest) first
    asks.sort(key=lambda l: l[0])   # best (lowest) first

    def cumulative(levels):
        total, out = 0, []
        for price, qty in levels:
            total += qty
            out.append((price, total))
        return out

    def ladder(levels):
        """Staircase from the touch outwards: up at each price, then flat to the next price."""
        xs, ys, prev = [levels[0][0]], [0], 0
        for i, (price, total) in enumerate(levels):
            xs += [price, levels[i + 1][0] if i + 1 < len(levels) else price]
            ys += [total, total]
            prev = total
        return xs, ys

    cb, ca = cumulative(bids), cumulative(asks)
    bx, by = ladder(cb)
    ax_, ay = ladder(ca)
    fig, ax = plt.subplots(figsize=(8, 4.2), dpi=160)
    ax.plot(bx, by, color="#1a7f4b", lw=2, label="bids")
    ax.fill_between(bx, by, color="#1a7f4b", alpha=0.18)
    ax.plot(ax_, ay, color="#c0392b", lw=2, label="asks")
    ax.fill_between(ax_, ay, color="#c0392b", alpha=0.18)
    spread = ca[0][0] - cb[0][0]
    ax.axvline((ca[0][0] + cb[0][0]) / 2, color="#555", lw=0.8, ls="--")
    ax.set_title(f"Order book depth (synthetic market, spread {spread:.2f})", fontsize=11)
    ax.set_xlabel("price")
    ax.set_ylabel("cumulative shares")
    ax.grid(alpha=0.25)
    ax.legend(frameon=False)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    fig.tight_layout()
    fig.savefig(dst)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
