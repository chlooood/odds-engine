#pragma once
#include <cstdint>

// Fixed-size, trivially-copyable wire message for one book's quote on one market.
// Exactly one cache line (64 bytes) by design — see docs/notes.md.
struct alignas(64) OddsUpdate {
    uint64_t seq_no;          // 8 bytes, monotonically increasing per book
    uint64_t sim_timestamp_ns;// 8 bytes, simulated nanosecond timestamp
    uint16_t book_id;         // 2 bytes
    uint16_t market_id;       // 2 bytes
    uint8_t  outcome_count;   // 1 byte, e.g. 2 (moneyline) or 3 (1X2)
    uint8_t  _pad0[3];        // explicit padding to keep odds_milli 4-byte aligned
    uint32_t odds_milli[4];   // 16 bytes, decimal odds * 1000 per outcome (unused slots = 0)
    uint8_t  _pad1[24];       // pad out the remainder to fill the 64-byte cache line
};

static_assert(sizeof(OddsUpdate) == 64, "OddsUpdate must be exactly one cache line");
static_assert(alignof(OddsUpdate) == 64, "OddsUpdate must be 64-byte aligned");
