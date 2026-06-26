#!/usr/bin/env python3
# Author: Harsh
import sys
import re
import os
import matplotlib.pyplot as plt

def parse_log(file_path):
    metrics = {}
    pattern = re.compile(r'p(50|95|99|99\.9)\s+latency:\s+([\d.]+)\s+ns')
    with open(file_path, 'r') as f:
        for line in f:
            match = pattern.search(line)
            if match:
                key = f"p{match.group(1)}"
                val = float(match.group(2))
                if key not in metrics:
                    metrics[key] = val
    return metrics

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 plot_latency.py <benchmark_output.txt>")
        sys.exit(1)
        
    file_path = sys.argv[1]
    if not os.path.exists(file_path):
        print(f"File not found: {file_path}")
        sys.exit(1)
        
    metrics = parse_log(file_path)
    if not metrics:
        print("No latency metrics found in file.")
        sys.exit(1)
        
    print("Latency Summary Table")
    print("-" * 30)
    for k, v in metrics.items():
        print(f"{k:10} | {v:10.3f} ns")
    print("-" * 30)
    
    os.makedirs('results', exist_ok=True)
    
    labels = list(metrics.keys())
    values = list(metrics.values())
    
    plt.figure(figsize=(8, 5))
    plt.bar(labels, values, color=['blue', 'orange', 'green', 'red'])
    plt.xlabel('Percentiles')
    plt.ylabel('Latency (ns)')
    plt.title('Latency Percentiles')
    plt.grid(axis='y', linestyle='--', alpha=0.7)
    
    out_path = os.path.join('results', 'latency_chart.png')
    plt.savefig(out_path)
    print(f"Chart saved to {out_path}")

if __name__ == "__main__":
    main()
