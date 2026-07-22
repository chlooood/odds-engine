#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "../sim/LatentMarket.hpp"

using Catch::Matchers::WithinAbs;

TEST_CASE("latent probabilities always valid", "[latent]") {
    LatentMarket m(42, {0.45, 0.27, 0.28});
    for (int tick = 0; tick < 20000; ++tick) {
        auto p = m.step();
        double sum = 0.0;
        for (int i = 0; i < LatentMarket::kOutcomes; ++i) {
            REQUIRE(p[i] > 0.0);
            REQUIRE(p[i] < 1.0);
            sum += p[i];
        }
        REQUIRE_THAT(sum, WithinAbs(1.0, 1e-12));
    }
}

TEST_CASE("process does not collapse to certainty", "[latent]") {
    for (uint64_t seed = 1; seed <= 20; ++seed) {
        LatentMarket m(seed, {0.45, 0.27, 0.28});
        double peak = 0.0;
        for (int tick = 0; tick < 20000; ++tick) {
            auto p = m.step();
            for (int i = 0; i < LatentMarket::kOutcomes; ++i)
                peak = std::max(peak, p[i]);
        }
        REQUIRE(peak < 0.995);
    }
}

TEST_CASE("same seed reproduces same path", "[latent]") {
    LatentMarket a(7, {0.4, 0.3, 0.3});
    LatentMarket b(7, {0.4, 0.3, 0.3});
    for (int tick = 0; tick < 1000; ++tick) {
        auto pa = a.step();
        auto pb = b.step();
        for (int i = 0; i < LatentMarket::kOutcomes; ++i)
            REQUIRE(pa[i] == pb[i]);
    }
}
