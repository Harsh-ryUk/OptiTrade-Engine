# Benchmarking

`ot_bench` measures **tick-to-decision** latency: the time `Engine::on_itch()` takes for one market data
message, including the book update, the strategy callback, the risk check and, when the strategy trades,
building the order and handing it to a gateway. It does not include a NIC, the kernel network stack or
decoding from a socket, so it is *not* a wire-to-wire figure.

```bash
cmake --preset release && cmake --build --preset release --target ot_bench
./build/release/ot_bench --strategy all --messages 2000000 --csv bench.csv
./build/release/ot_bench --strategy imbalance --rate 2000000 --cpu 2     # open loop, pinned (Linux)
```

## Method

* The whole feed is generated in memory before timing starts (seeded synthetic market), so generation
  is never measured and every run sees identical input.
* A warm-up pass is discarded. Every measured pass uses a fresh engine.
* **Throughput pass:** the whole feed is timed once. The mean cost per message from this pass has no timer
  overhead or resolution problem and is the number to trust on machines with a coarse clock.
* **Closed-loop pass:** each call is timed individually (`lfence; rdtsc; lfence` calibrated against
  `steady_clock` on x86-64, `steady_clock` elsewhere). The timer's read floor and step are measured and printed.
* **Open-loop pass** (`--rate EPS`): message *i* is scheduled at `t0 + i / rate`. Two latencies are reported:
  *service time* (`end - start`) and *response time* (`end - intended start`). If one call stalls, the
  messages queued behind it are charged for the wait. Reporting service time alone hides this and is the
  classic coordinated-omission mistake.
* Percentiles are exact (sorted samples), not bucketed.
* The engine's counters must be identical in every pass; a mismatch is reported as an error. The benchmark
  also answers the strategy's orders with exchange reports (outside the timed region) so strategies keep
  trading; `--no-reports` disables that.

## Result committed with the repository

`results/bench_apple_m1.{txt,csv}`: Apple M1, macOS, Apple clang, Release, **not pinned**, an ordinary
desktop with other processes running (load average about 6), 2 000 000 measured messages per strategy on an
8-symbol synthetic market, closed loop.

| Strategy | Mean cost per message (whole-loop pass) | Messages per second | Orders sent |
|---|---:|---:|---:|
| Imbalance taker | 81.6 ns | 12.3 M | 26 967 |
| Microprice maker | 78.0 ns | 12.8 M | 436 new, 436 cancels, 50 658 replaces |
| EMA crossover | 70.1 ns | 14.3 M | 26 871 |

The throughput pass includes answering orders with exchange reports, so it slightly overstates the cost of
the engine alone.

## Reading the numbers honestly

* **Timer resolution.** Apple Silicon's counter ticks at 24 MHz, i.e. every ~41.7 ns. Per-call percentiles
  below a few steps only say which step the call landed in; the printed note says so. Use the whole-loop
  mean there.
* **Tails are mostly the machine.** On an unpinned desktop the maximum and p99.99 are dominated by the
  scheduler (single stalls of tens of microseconds). That is what the open-loop response time exists to show.
  For publishable tail numbers run on bare metal with an isolated core (`isolcpus`, `nohz_full`, `--cpu`).
* Numbers from different machines, compilers or operating systems are not comparable, and these figures
  have not been measured on x86 hardware.
* CI runs the benchmark on every push as an artifact on shared runners; treat those as indicative only.
