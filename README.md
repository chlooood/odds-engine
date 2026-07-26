# odds-engine

A low-latency odds arbitrage engine built in modern C++, developed in stages.
The system generates synthetic multi-book betting markets, then detects and
sizes bets against structural mispricings between books.

## Status

- [x] Stage 0: Market simulator, wire format, deterministic core, test/CI
- [x] Stage 1: Devig, consensus fair price, Kelly sizing, replay hash
- [x] Stage 2: Live transport (UDP), WebSocket dashboard, live configuration
- [ ] Stage 3: Performance pass (lock-free queues, benchmarking), real-feed bridge

## What it does

Different bookmakers update their odds at different speeds. A slow book can be
quoting a stale price seconds after the true probability has moved. In that
window, the stale price is exploitable. This project builds, from scratch:

1. A market simulator that manufactures this inefficiency on purpose, with a
   hidden true-probability process and multiple books that observe it with
   configurable lag, noise, and margin.
2. An engine that strips each book's margin, estimates a consensus fair price,
   and sizes positions with fractional Kelly.
3. A live feed: the simulator broadcasts over UDP; the engine joins mid-stream,
   sizes from periodic announce datagrams, and streams fair prices to a browser
   dashboard over WebSocket. Configuration (devig method, staleness threshold,
   Kelly fraction, exposure cap) can be changed mid-run from the browser.
4. Offline analytics: closing-line value, realised PnL, and a post-jump edge
   profile that shows how edge concentrates after latent-process jumps and
   quantifies the effective independent sample driving the estimate.

Because the simulator knows the ground-truth probability, the engine's estimates
can be validated against truth, which real-data projects cannot do.

## Build

Requires CMake 3.20+ and a C++20 compiler (GCC 11+ or Clang 14+).

    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel
    ctest --test-dir build --output-on-failure

## Try it

### File mode

    ./build/odds_sim --seed 42 --markets 5 --ticks 10000 --out data/session.bin
    ./build/odds_engine --in data/session.bin --bets data/bets.bin --summary-every 1000
    ./build/verify_session data/session.bin

Same seed produces byte-identical output every run.

### Live mode

    # Terminal 1: broadcast at 50x real time
    ./build/odds_sim --seed 42 --markets 10 --ticks 20000 \
      --out data/live.bin --udp 127.0.0.1:9000 --pace 50

    # Terminal 2: consume feed, serve dashboard
    ./build/odds_engine --listen 9000 --ws 8080 --idle-ms 5000

    # Browser: http://127.0.0.1:8080
    # Use the control panel to change devig method, staleness, or Kelly fraction live.

### Analytics

    ./build/odds_sim --seed 42 --markets 200 --ticks 5000 \
      --out data/s.bin --settle data/s.settle --jumps data/s.jumps
    ./build/odds_engine --in data/s.bin --bets data/s.bets --summary-every 0
    ./build/analyze_bets --bets data/s.bets --session data/s.bin \
      --settle data/s.settle --jumps data/s.jumps

## Design highlights

- 64-byte cache-line-aligned messages: reading one costs a single cache-line
  fill and never straddles a line.
- Deterministic seeding via splitmix64-derived sub-seeds: same seed, byte-
  identical output, checked in CI. The replay hash extends this to the engine's
  fair-price trajectory.
- Transport abstraction: the simulator writes to a file and broadcasts over UDP
  simultaneously without touching the generation loop.
- Engine templated over its source: SessionReader and UdpSource expose the same
  interface; the read path has zero virtual calls.
- Single-threaded WebSocket server polled from the engine loop: no threads, no
  shared state, no lock.

## Roadmap

- [x] Stage 0: Market simulator, wire format, deterministic core, test/CI
- [x] Stage 1: Devig, consensus fair price, Kelly sizing
- [x] Stage 2: Live transport (UDP), WebSocket server, web dashboard, live config
- [ ] Stage 3: Performance pass (lock-free queues, benchmarking), real-feed bridge

## License

MIT
