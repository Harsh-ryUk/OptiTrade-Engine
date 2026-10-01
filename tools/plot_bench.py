#!/usr/bin/env python3
"""Chart `ot_bench --csv` output: mean cost per message and the closed-loop latency percentiles.

    ./build/release/ot_bench --strategy all --csv bench.csv
    python3 tools/plot_bench.py bench.csv docs/img/latency.png
"""
import csv
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

COLORS = {"imbalance_taker": "#2563eb", "microprice_maker": "#16a34a", "ema_cross": "#d97706"}
PCTS = [("p50_ns", "p50"), ("p90_ns", "p90"), ("p99_ns", "p99"), ("p999_ns", "p99.9"), ("p9999_ns", "p99.99")]


def main(src, dst):
    series, mean_cost = {}, {}
    with open(src) as f:
        for row in csv.DictReader(f):
            name = row["strategy"]
            if row["pass"] == "closed" and row["metric"] == "service":
                series[name] = [float(row[k]) for k, _ in PCTS]
            if row["pass"] == "throughput":
                mean_cost[name] = float(row["mean_ns"])
    if not series:
        sys.exit("no closed-loop rows found")

    fig, (a1, a2) = plt.subplots(1, 2, figsize=(10, 4), dpi=160, gridspec_kw={"width_ratios": [1, 1.5]})
    names = list(mean_cost)
    a1.bar(range(len(names)), [mean_cost[n] for n in names], color=[COLORS.get(n, "#555") for n in names])
    a1.set_xticks(range(len(names)))
    a1.set_xticklabels([n.replace("_", "\n") for n in names], fontsize=8)
    a1.set_ylabel("ns per message (whole-loop mean)")
    a1.set_title("Mean cost per message", fontsize=10)
    for i, n in enumerate(names):
        a1.text(i, mean_cost[n] + 1, f"{mean_cost[n]:.0f} ns", ha="center", fontsize=8)

    for n, vals in series.items():
        a2.plot([lab for _, lab in PCTS], vals, marker="o", lw=2, color=COLORS.get(n, "#555"), label=n)
    a2.set_yscale("log")
    ticks = [50, 100, 200, 500, 1000, 2000]
    a2.set_yticks(ticks)
    a2.set_yticklabels([str(t) for t in ticks])
    a2.minorticks_off()
    a2.set_ylabel("ns (log scale)")
    a2.set_title("Per-call latency percentiles (closed loop)", fontsize=10)
    a2.grid(alpha=0.25, which="both")
    a2.legend(frameon=False, fontsize=8)
    fig.text(0.5, 0.015, "Engine::on_itch on an Apple M1, unpinned, 2 M messages. The timer steps in 41.7 ns, "
             "so the lowest percentiles are quantised.", ha="center", fontsize=8)
    for ax in (a1, a2):
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)
    fig.tight_layout(rect=(0, 0.06, 1, 1))
    fig.savefig(dst)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
