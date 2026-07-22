#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include "../proto/messages.hpp"

// Per-(market, book) quote state, held in a flat 2-D array rather than a map.
// This is the engine's hot path: apply() runs once per incoming OddsUpdate and
// must not allocate, throw, or take a lock. Rejection is communicated through
// a return code and a counter, never an exception.
//
// MaxMarkets/MaxBooks are template parameters, not the wire-format ceilings in
// proto/messages.hpp (65535 each). market_id/book_id are uint16_t on the wire
// because that is the hard protocol limit; the table itself is sized to what a
// single engine process actually needs to hold, since MaxMarkets * MaxBooks
// Quotes are allocated as a plain member array, not on the heap. Defaults here
// match the simulator's default roster (5 books) with headroom.
template <size_t MaxMarkets = 64, size_t MaxBooks = 16>
class BookTable {
public:
    struct alignas(64) Quote {
        uint32_t odds_milli[4]{};
        uint64_t recv_timestamp_ns = 0;
        // kEmptySeq marks a slot that has never been written. Using a
        // sentinel instead of a separate "valid" bool keeps the struct the
        // same size and makes "never written" and "stale" the same check in
        // is_stale(): both compare seq_no/recv_timestamp_ns, nothing else.
        uint64_t seq_no = kEmptySeq;
        uint8_t outcome_count = 0;
    };

    static constexpr uint64_t kEmptySeq = std::numeric_limits<uint64_t>::max();

    enum class ApplyResult : uint8_t {
        Applied,
        RejectedOutOfRange, // market_id or book_id >= the table's bound
        RejectedStaleSeq,   // seq_no did not advance for this (market, book)
    };

    // No allocation, no exceptions: a malformed or replayed record is a
    // routine occurrence on a live feed, not a program error, so it is
    // reported through the return value and a counter instead of throwing.
    ApplyResult apply(const OddsUpdate& u, uint64_t recv_ns) {
        if (u.market_id >= MaxMarkets || u.book_id >= MaxBooks) {
            ++rejected_out_of_range_;
            return ApplyResult::RejectedOutOfRange;
        }
        Quote& slot = table_[u.market_id][u.book_id];
        // kEmptySeq never compares less than a real seq_no (they start at 0),
        // so an unwritten slot always accepts its first update without a
        // separate branch.
        if (slot.seq_no != kEmptySeq && u.seq_no <= slot.seq_no) {
            ++rejected_stale_seq_;
            return ApplyResult::RejectedStaleSeq;
        }
        slot.recv_timestamp_ns = recv_ns;
        slot.seq_no = u.seq_no;
        slot.outcome_count = u.outcome_count;
        for (int i = 0; i < 4; ++i) slot.odds_milli[i] = u.odds_milli[i];
        return ApplyResult::Applied;
    }

    // Caller's responsibility to pass in-range ids, exactly as with a raw
    // array index. The main loop only ever calls get() with ids it already
    // validated through apply() or through the session header's market/book
    // counts, so an assert here would fire on a caller bug, not on bad feed
    // data - bad feed data is apply()'s problem, not get()'s.
    const Quote& get(uint16_t market_id, uint16_t book_id) const {
        return table_[market_id][book_id];
    }

    // A slot that was never written is always stale, by construction of
    // kEmptySeq above. stale_after_ns is passed per call rather than stored,
    // so the same table can be queried under different staleness policies
    // (e.g. a tighter threshold right after a detected jump) without needing
    // a second table.
    bool is_stale(uint16_t market_id, uint16_t book_id, uint64_t now_ns,
                  uint64_t stale_after_ns) const {
        const Quote& q = table_[market_id][book_id];
        if (q.seq_no == kEmptySeq) return true;
        // now_ns is allowed to be less than recv_timestamp_ns in a replay or
        // out-of-order test harness; treat that as "not yet stale" rather
        // than underflowing the unsigned subtraction.
        if (now_ns <= q.recv_timestamp_ns) return false;
        return (now_ns - q.recv_timestamp_ns) >= stale_after_ns;
    }

    uint64_t rejected_out_of_range() const { return rejected_out_of_range_; }
    uint64_t rejected_stale_seq() const { return rejected_stale_seq_; }

    static constexpr size_t max_markets() { return MaxMarkets; }
    static constexpr size_t max_books() { return MaxBooks; }

private:
    std::array<std::array<Quote, MaxBooks>, MaxMarkets> table_{};
    uint64_t rejected_out_of_range_ = 0;
    uint64_t rejected_stale_seq_ = 0;
};
