#pragma once
#include <cstdint>

// Simulated tick interval. The simulator advances time by this much per tick;
// every consumer that needs to recover a tick index from a wire timestamp
// divides by it. Single definition, included where needed: no anonymous-
// namespace copies, which is what made this ambiguous once already.
inline constexpr uint64_t kTickIntervalNs = 100'000'000ULL; // 100 ms
