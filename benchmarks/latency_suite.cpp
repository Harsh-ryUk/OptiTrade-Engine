#include <iostream>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cstdint>
#include <x86intrin.h>

#include "../include/order_book.hpp"

namespace {
    constexpr std::size_t kWarmupIterations = 10'000;
    constexpr std::size_t kNumIterations = 1'000'000;
    
    // Pre-allocated array in BSS segment to avoid heap allocations in the hot path.
    std::uint64_t latencies[kNumIterations];
}

int main() {
    // 1000 Price levels, 2,000,000 max orders
    optitrade::OrderBook<1000, 2'000'000> book;
    
    std::cout << "Warming up OrderBook...\n";
    // Warmup phase
    for (std::size_t i = 0; i < kWarmupIterations; ++i) {
        auto* order = book.add_order(i, optitrade::Side::buy, i % 1000, 100);
        book.cancel_order(order);
    }
    
    std::cout << "Running latency benchmark...\n";
    
    // Benchmark phase: Measure Order Insertion
    for (std::size_t i = 0; i < kNumIterations; ++i) {
        // CPU instruction barrier to prevent reordering
        _mm_lfence();
        std::uint64_t start = __rdtsc();
        _mm_lfence();
        
        auto* order = book.add_order(i, optitrade::Side::buy, i % 1000, 100);
        
        _mm_lfence();
        std::uint64_t end = __rdtsc();
        _mm_lfence();
        
        latencies[i] = end - start;
        
        // Not included in the insertion latency measurement
        book.cancel_order(order); 
    }
    
    std::sort(std::begin(latencies), std::end(latencies));
    
    std::uint64_t sum = std::accumulate(std::begin(latencies), std::end(latencies), 0ULL);
    double mean = static_cast<double>(sum) / kNumIterations;
    
    std::cout << "\n=========================================\n";
    std::cout << "Order Insert Latency (CPU Cycles)\n";
    std::cout << "=========================================\n";
    std::cout << "  Iterations: " << kNumIterations << "\n";
    std::cout << "  Mean:       " << mean << " cycles\n";
    std::cout << "  Min:        " << latencies[0] << " cycles\n";
    std::cout << "  p50 (Med):  " << latencies[kNumIterations / 2] << " cycles\n";
    std::cout << "  p90:        " << latencies[(kNumIterations * 90) / 100] << " cycles\n";
    std::cout << "  p99:        " << latencies[(kNumIterations * 99) / 100] << " cycles\n";
    std::cout << "  p99.9:      " << latencies[(kNumIterations * 999) / 1000] << " cycles\n";
    std::cout << "  Max:        " << latencies[kNumIterations - 1] << " cycles\n";
    std::cout << "=========================================\n";
    
    return 0;
}
