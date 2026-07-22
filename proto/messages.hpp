#pragma once
#include <cstdint>

// Fixed-size, trivially-copyable wire message for one book's quote on one market.
// Exactly one cache line by design: a message never straddles two lines, and
// adjacent messages cannot false-share once producer and consumer are on
// different cores.
struct alignas(64) OddsUpdate {
    uint64_t seq_no;
    uint64_t sim_timestamp_ns;
    uint16_t book_id;
    uint16_t market_id;
    uint8_t  outcome_count;
    uint8_t  _pad0[3];
    uint32_t odds_milli[4];   // decimal odds x 1000; slots >= outcome_count are zero
    uint8_t  _pad1[24];
};

static_assert(sizeof(OddsUpdate) == 64, "OddsUpdate must be exactly one cache line");
static_assert(alignof(OddsUpdate) == 64, "OddsUpdate must be 64-byte aligned");

// market_id and book_id are uint16_t on the wire, so these are hard ceilings,
// not tuning parameters.
inline constexpr uint32_t kMaxMarketsSupported = 65535;
inline constexpr uint32_t kMaxBooksSupported   = 65535;

// Shared between sim/main.cpp (which advances simulated time by this amount
// per tick) and tools/validate_devig.cpp (which must invert that mapping to
// recover the tick index from a wire timestamp). Defined once here so the two
// can never drift apart.
inline constexpr uint64_t kTickIntervalNs = 100'000'000ULL; // 100 ms
