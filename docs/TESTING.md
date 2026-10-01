# Testing and verification

Correctness is the point of this project, so the test suite is larger than the code it tests.

```bash
cmake --preset release && cmake --build --preset release && ctest --preset release
cmake --preset asan    && cmake --build --preset asan    && ctest --preset asan     # ASan + UBSan
cmake --preset tsan    && cmake --build --preset tsan    && ctest --preset tsan     # ThreadSanitizer
cmake --preset fuzz    && cmake --build --preset fuzz                               # libFuzzer (clang)
```

Every `tests/test_*.cpp` is its own executable and its own ctest entry (see `tests/check.hpp`, a
dependency-free harness).

## What kinds of tests exist

| Kind | Where | What it protects against |
|---|---|---|
| Known-answer vectors | `test_itch_*`, `test_ouch_*`, `test_mold64`, `test_risk_engine`, `test_exchange_sim` | Byte layouts typed by hand from the specs (not produced by the project's own encoder, so a symmetric bug cannot hide), and hand-computed PnL, fills and queue positions |
| Differential tests | `test_book_differential`, `test_core_flat_hash_map_differential` | Fast data structures compared after *every* operation with a naive `std::map` / `std::unordered_map` model over hundreds of thousands of random operations, including invalid ones |
| Model-based tests | `test_oms_model` | A naive exchange that produces legal and illegal report sequences (races, duplicates, reordering, forged tokens); invariants such as "risk exposure equals the sum of open quantities" are checked after every step |
| Differential tests (ladder) | `test_book_ladder_window` | Books with hundreds of levels and operations at every distance from the best price, compared with a `std::map` reference after every operation; also depths around the search-window boundary |
| Hint tests | `test_book_prefetch` | Cache hints never change results and accept any bytes |
| Property tests | `test_synthetic_market`, `test_strategy_*` | Generated markets never cross or reference unknown orders; strategies never emit invalid orders and are deterministic |
| Fuzz smoke tests | `test_*_fuzz_smoke` | The libFuzzer property bodies run on every ctest with deterministic pseudo-random and mutated input |
| libFuzzer targets | `fuzz/fuzz_*.cpp` | Coverage-guided fuzzing of the ITCH, OUCH and MoldUDP64 decoders and of the order book fed by a fuzzed ITCH stream. |
| Concurrency | `test_core_spsc_ring`, `test_udp_pipeline` | Wait-free ring (5 M items across threads) and the three-thread UDP pipeline, also run under ThreadSanitizer |
| Allocation | `test_engine` | A replaced `operator new` proves a stretch of engine message processing allocates nothing (skipped under sanitizers, which bring their own allocator) |
| Determinism | `test_determinism` | Frozen digests of complete backtests; see below |

## Determinism

Backtests are bit-reproducible: all decision arithmetic is integer, the random generator is a hand
written xoshiro256**, and nothing reads the clock. `test_determinism` runs seed 42, 8 symbols, 200 000
messages through every strategy and compares an FNV-1a digest of every order sent and every exchange
report received with constants recorded in the test. The same constants pass on macOS (Apple clang), Linux
gcc and Linux clang; CI repeats the gcc/clang comparison on every push.

## Mutation checking

While the test suite was written, each module's tests were attacked by hand-made and automatic
mutations of the headers (flipped comparisons, off-by-one boundaries, dropped checks). A mutant that
survived the tests pointed at a missing assertion, and the assertion was added. This is a development
practice, not part of CI.

## Independent review

Separate review passes tried to break each area with their own reference models and reproducers. They found
real defects in the order manager (open-order accounting, dropped fills, duplicate executions, replace
checks bypassing limits, order-slot exhaustion) and in the exchange simulator (queue priority between
our own orders, fills outside a limit, double fill of a marketable remainder); each is fixed with a regression
test. The same passes confirmed the allocation, SPSC ring and benchmark-method claims.

## Real market data

The decoder, books, engine and simulator were run on Nasdaq's public ITCH 5.0 sample file for 30 December 2019
(8.25 GB, 268 744 780 messages, 8 906 instruments; results in `results/real_data_nasdaq_2019-12-30.txt`):

* No decode errors, unknown orders, duplicates or invalid updates across the whole day, and no live orders left at
  the end of the day (every order the feed added is accounted for by a later cancel, delete or execution).
* The run exposed three defects that synthetic data hid, now fixed with a regression test: placeholder quotes
  (extremely wide or stale bid/ask pairs in thin pre-market books) gave meaningless mid prices, a final re-marking pass
  valued a $41 position at about $100 000, and real feeds need a price band with a mandatory reference price.
* The price-level cap is a deliberate memory trade-off; at 1 024 levels per side 0.02 % of messages are refused (the
  backtester reports it).

## What is not covered

* Only one real trading day has been run (Nasdaq's public sample for 30 December 2019, see below); other
  days, other venues and live feeds are untested.
* Real hardware: all measurements so far are from ordinary machines, not isolated bare-metal cores or a NIC.
* The multicast data path is only exercised where the host allows it (tests skip otherwise).
