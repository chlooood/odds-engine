#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include "../proto/messages.hpp"

TEST_CASE("OddsUpdate layout is fixed", "[wire]") {
    STATIC_REQUIRE(sizeof(OddsUpdate) == 64);
    STATIC_REQUIRE(alignof(OddsUpdate) == 64);
    STATIC_REQUIRE(std::is_trivially_copyable_v<OddsUpdate>);
}

TEST_CASE("OddsUpdate round trip", "[wire]") {
    OddsUpdate out{};
    out.seq_no = 42;
    out.sim_timestamp_ns = 1234567890;
    out.book_id = 3;
    out.market_id = 7;
    out.outcome_count = 3;
    out.odds_milli[0] = 2150;
    out.odds_milli[1] = 3400;
    out.odds_milli[2] = 4100;
    unsigned char bytes[sizeof(OddsUpdate)];
    std::memcpy(bytes, &out, sizeof(out));
    OddsUpdate in{};
    std::memcpy(&in, bytes, sizeof(in));
    REQUIRE(in.seq_no == out.seq_no);
    REQUIRE(in.book_id == out.book_id);
    REQUIRE(in.market_id == out.market_id);
    REQUIRE(std::memcmp(in.odds_milli, out.odds_milli, sizeof(out.odds_milli)) == 0);
}

TEST_CASE("value-initialised OddsUpdate is all zeros", "[wire]") {
    OddsUpdate u{};
    unsigned char bytes[sizeof(OddsUpdate)];
    std::memcpy(bytes, &u, sizeof(u));
    for (size_t i = 0; i < sizeof(bytes); ++i) REQUIRE(bytes[i] == 0);
}
