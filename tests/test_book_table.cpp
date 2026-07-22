#include <catch2/catch_test_macros.hpp>
#include <limits>
#include "../engine/BookTable.hpp"

namespace {
OddsUpdate make_update(uint16_t market, uint16_t book, uint64_t seq, uint32_t o0 = 2000) {
    OddsUpdate u{};
    u.seq_no = seq;
    u.sim_timestamp_ns = 0;
    u.market_id = market;
    u.book_id = book;
    u.outcome_count = 3;
    u.odds_milli[0] = o0;
    u.odds_milli[1] = 3000;
    u.odds_milli[2] = 5000;
    return u;
}
using Table = BookTable<4, 4>;
}

TEST_CASE("apply then get returns what was stored", "[booktable]") {
    Table t;
    auto r = t.apply(make_update(1, 2, 10, 1234), /*recv_ns=*/500);
    REQUIRE(r == Table::ApplyResult::Applied);
    const auto& q = t.get(1, 2);
    REQUIRE(q.seq_no == 10);
    REQUIRE(q.recv_timestamp_ns == 500);
    REQUIRE(q.odds_milli[0] == 1234);
    REQUIRE(q.outcome_count == 3);
}

TEST_CASE("older or equal seq_no is rejected and ignored", "[booktable]") {
    Table t;
    REQUIRE(t.apply(make_update(0, 0, 10, 1000), 100) == Table::ApplyResult::Applied);
    // Equal seq_no: not an advance, must be rejected.
    REQUIRE(t.apply(make_update(0, 0, 10, 9999), 200) == Table::ApplyResult::RejectedStaleSeq);
    // Strictly older seq_no: must be rejected.
    REQUIRE(t.apply(make_update(0, 0, 5, 8888), 300) == Table::ApplyResult::RejectedStaleSeq);
    // The stored value must be untouched by either rejected write.
    const auto& q = t.get(0, 0);
    REQUIRE(q.seq_no == 10);
    REQUIRE(q.odds_milli[0] == 1000);
    REQUIRE(t.rejected_stale_seq() == 2);
}

TEST_CASE("out-of-range market_id or book_id is rejected, not undefined behaviour", "[booktable]") {
    Table t; // 4x4
    REQUIRE(t.apply(make_update(4, 0, 1), 0) == Table::ApplyResult::RejectedOutOfRange);
    REQUIRE(t.apply(make_update(0, 4, 1), 0) == Table::ApplyResult::RejectedOutOfRange);
    REQUIRE(t.rejected_out_of_range() == 2);
}

TEST_CASE("a never-written slot is always stale", "[booktable]") {
    Table t;
    REQUIRE(t.is_stale(0, 0, /*now_ns=*/0, /*stale_after_ns=*/1'000'000) == true);
    REQUIRE(t.is_stale(0, 0, /*now_ns=*/std::numeric_limits<uint64_t>::max(), 1) == true);
}

TEST_CASE("staleness flips at exactly the threshold", "[booktable]") {
    Table t;
    t.apply(make_update(0, 0, 1), /*recv_ns=*/1000);
    constexpr uint64_t kThreshold = 500;
    // Just under threshold: not stale yet.
    REQUIRE(t.is_stale(0, 0, /*now_ns=*/1000 + kThreshold - 1, kThreshold) == false);
    // Exactly at threshold: stale (the boundary is inclusive on the stale side).
    REQUIRE(t.is_stale(0, 0, /*now_ns=*/1000 + kThreshold, kThreshold) == true);
    // Well past threshold: stale.
    REQUIRE(t.is_stale(0, 0, /*now_ns=*/1000 + kThreshold + 1, kThreshold) == true);
}

TEST_CASE("now_ns at or before recv_timestamp_ns is never stale", "[booktable]") {
    Table t;
    t.apply(make_update(0, 0, 1), /*recv_ns=*/1000);
    REQUIRE(t.is_stale(0, 0, /*now_ns=*/1000, 0) == false);
    REQUIRE(t.is_stale(0, 0, /*now_ns=*/500, 0) == false); // out-of-order now_ns, must not underflow
}
