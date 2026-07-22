# odds-engine

A low-latency odds arbitrage engine built in modern C++, developed in stages.
The system generates synthetic multi-book betting markets, then (in later stages)
detects and sizes bets against structural mispricings between books.

Status: Stage 0 complete (market simulator and core data structures).
Stage 1 (engine math) in progress.

## What it does

Different bookmakers update their odds at different speeds. A slow book can be
quoting a stale price seconds after the true probability has moved. In that
window, the stale price is exploitable. This project builds, from scratch:

1. A market simulator that manufactures this inefficiency on purpose, with a
   hidden true-probability process and multiple books that observe it with
   configurable lag, noise, and margin.
2. An engine (in progress) that strips each book's margin, estimates a
   consensus fair price, and sizes positions with fractional Kelly.
3. A dashboard (planned) to configure and monitor the engine live.

Because the simulator knows the ground-truth probability, the engine's estimates
can be validated against truth, which real-data projects cannot do.

## Build

Requires CMake 3.20+ and a C++20 compiler (GCC 11+ or Clang 14+).

    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel
    ctest --test-dir build --output-on-failure

## Try it

    ./build/odds_sim --seed 42 --markets 5 --ticks 10000 --out data/session.bin
    ./build/verify_session data/session.bin

Same seed produces byte-identical output every run.

## Design highlights

- 64-byte cache-line-aligned messages: reading one costs a single cache-line
  fill and never straddles a line.
- Deterministic seeding via splitmix64-derived sub-seeds, so runs are
  byte-reproducible and refactors can be checked with a replay hash.
- Transport abstraction so the simulator can write to a file today and broadcast
  over a socket later without touching generation logic.

## Roadmap

- [x] Stage 0: Market simulator, wire format, deterministic core, test/CI
- [ ] Stage 1: Devig, consensus fair price, Kelly sizing
- [ ] Stage 2: Live transport (UDP), WebSocket server, web dashboard
- [ ] Stage 3: Performance pass (lock-free queues, benchmarking), real-feed bridge

## License

MIT
