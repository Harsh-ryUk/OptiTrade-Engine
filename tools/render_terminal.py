#!/usr/bin/env python3
"""Render captured command output as a terminal-style PNG for the README.

    cmd | python3 tools/render_terminal.py "title or command line" out.png [highlight-substring ...]
"""
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


def main(title, dst, highlights):
    lines = sys.stdin.read().rstrip("\n").split("\n")
    width = max(len(l) for l in lines + [title]) + 4
    height = len(lines) + 3
    fig = plt.figure(figsize=(max(6, width * 0.085), max(2, height * 0.2)), dpi=160)
    fig.patch.set_facecolor("#1e1e2e")
    ax = fig.add_axes([0, 0, 1, 1])
    ax.set_facecolor("#1e1e2e")
    ax.axis("off")
    ax.set_xlim(0, width)
    ax.set_ylim(height, 0)
    ax.text(1, 0.9, "$ " + title, color="#a6e3a1", family="monospace", fontsize=8, va="center")
    for i, line in enumerate(lines):
        hot = any(h in line for h in highlights)
        ax.text(1, i + 2.1, line, color="#f9e2af" if hot else "#cdd6f4", family="monospace",
                fontsize=8, va="center", fontweight="bold" if hot else "normal")
    fig.savefig(dst, facecolor=fig.get_facecolor())


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2], sys.argv[3:])
