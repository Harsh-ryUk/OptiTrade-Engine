#!/usr/bin/env python3

import sys
import re
import matplotlib.pyplot as plt

def parse_benchmark_output(filepath):
    percentiles = []
    latencies = []
    
    with open(filepath, 'r') as f:
        for line in f:
            match = re.search(r'(Mean|p50|p95|p99|p99\.9|Maximum)\s+latency:\s+([\d.]+)\s+ns', line)
            if match:
                metric = match.group(1)
                latency = float(match.group(2))
                percentiles.append(metric)
                latencies.append(latency)
    
    return percentiles, latencies

def plot_latency(percentiles, latencies, output_file="latency_plot.png"):
    if not percentiles:
        print("No latency data found in the provided file.")
        return

    plt.figure(figsize=(10, 6))
    plt.bar(percentiles, latencies, color='skyblue')
    plt.xlabel('Metric')
    plt.ylabel('Latency (ns)')
    plt.title('OptiTrade Engine - Latency Benchmark Results')
    plt.yscale('log')
    plt.grid(axis='y', linestyle='--', alpha=0.7)
    
    for i, v in enumerate(latencies):
        plt.text(i, v, f"{v:.1f}", ha='center', va='bottom')

    plt.savefig(output_file)
    print(f"Plot saved to {output_file}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python plot_latency.py <benchmark_output.txt>")
        sys.exit(1)
    
    filepath = sys.argv[1]
    percentiles, latencies = parse_benchmark_output(filepath)
    plot_latency(percentiles, latencies)
