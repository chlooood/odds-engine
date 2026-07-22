#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "../sim/Bookmaker.hpp"
#include "../sim/LatentMarket.hpp"

using Catch::Matchers::WithinAbs;

namespace {
double implied_sum(const std::array<uint32_t, 3>& odds) {
    double s = 0.0;
    for (int i = 0; i < 3; ++i) s += 1000.0 / (double)odds[i];
    return s;
}
}

TEST_CASE("quoted odds stay in guarded range", "[book]") {
    LatentMarket market(42, {0.45, 0.27, 0.28});
    Bookmaker book({4, 150, 0.080, 0.070, 1.12}, 1234);
    for (int tick = 0; tick < 5000; ++tick) {
        book.observe(market.step());
        auto o = book.quote();
        for (int i = 0; i < 3; ++i) {
            REQUIRE(o[i] >= 1001);
            REQUIRE(o[i] <= 1000000);
        }
    }
}

TEST_CASE("books charge approximately their configured overround", "[book]") {
    LatentMarket market(42, {0.45, 0.27, 0.28});
    Bookmaker sharp({0, 1, 0.020, 0.010, 1.00}, 100);
    Bookmaker soft ({4, 1, 0.080, 0.010, 1.00}, 200);
    for (int tick = 0; tick < 500; ++tick) {
        auto latent = market.step();
        sharp.observe(latent);
        soft.observe(latent);
        REQUIRE_THAT(implied_sum(sharp.quote()), WithinAbs(1.020, 0.003));
        REQUIRE_THAT(implied_sum(soft.quote()),  WithinAbs(1.080, 0.003));
    }
}

TEST_CASE("lagged book diverges from sharp book", "[book]") {
    LatentMarket market(42, {0.45, 0.27, 0.28});
    Bookmaker sharp({0,   1, 0.020, 0.0, 1.0}, 100);
    Bookmaker slow ({4, 200, 0.020, 0.0, 1.0}, 100);
    double max_gap = 0.0;
    for (int tick = 0; tick < 5000; ++tick) {
        auto latent = market.step();
        sharp.observe(latent);
        slow.observe(latent);
        auto a = sharp.quote();
        auto b = slow.quote();
        for (int i = 0; i < 3; ++i) {
            double gap = std::abs(1000.0/a[i] - 1000.0/b[i]);
            max_gap = std::max(max_gap, gap);
        }
    }
    REQUIRE(max_gap > 0.10);
}
