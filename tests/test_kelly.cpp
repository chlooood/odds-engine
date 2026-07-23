#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <array>
#include <cmath>
#include <limits>
#include <vector>
#include "../engine/Kelly.hpp"

using Catch::Matchers::WithinAbs;
using kelly::allocate;

namespace {

// Independent reference: brute-force the optimal FULL-Kelly allocation by
// enumerating every subset of outcomes, computing the closed-form stakes and
// expected log wealth for each, and taking the best. This shares no code path
// with the greedy Smoczynski-Tomkins selection in Kelly.hpp, so agreement is
// evidence, not circular confirmation. Only valid for small n (subset count
// is 2^n), which is exactly the kMaxOutcomes <= 4 regime.
struct BruteResult {
    std::array<double, 4> stake{};
    double reserve = 1.0;
    bool bet = false;
};

double expected_log_wealth(const std::vector<double>& p, const std::vector<double>& o,
                           const std::array<double, 4>& f) {
    double reserve = 1.0;
    for (size_t i = 0; i < p.size(); ++i) reserve -= f[i];
    double e = 0.0;
    for (size_t j = 0; j < p.size(); ++j) {
        const double wealth = reserve + f[j] * o[j]; // f[j]==0 for unbet outcomes
        if (wealth <= 0.0) return -std::numeric_limits<double>::infinity();
        e += p[j] * std::log(wealth);
    }
    return e;
}

BruteResult brute_force(const std::vector<double>& p, const std::vector<double>& o) {
    const size_t n = p.size();
    BruteResult best{};
    double best_e = 0.0; // the "bet nothing" baseline: log(1) == 0

    for (unsigned mask = 1; mask < (1u << n); ++mask) {
        double P = 0.0, B = 0.0;
        for (size_t i = 0; i < n; ++i) {
            if (mask & (1u << i)) { P += p[i]; B += 1.0 / o[i]; }
        }
        const double denom = 1.0 - B;
        if (denom <= 0.0) continue;
        const double R = (1.0 - P) / denom;
        if (R < 0.0 || R > 1.0) continue;

        std::array<double, 4> f{};
        bool feasible = true;
        for (size_t i = 0; i < n; ++i) {
            if (mask & (1u << i)) {
                const double fi = p[i] - R / o[i];
                if (fi <= 0.0) { feasible = false; break; } // subset not self-consistent
                f[i] = fi;
            }
        }
        if (!feasible) continue;

        const double e = expected_log_wealth(p, o, f);
        if (e > best_e + 1e-15) {
            best_e = e;
            best.stake = f;
            best.reserve = R;
            best.bet = true;
        }
    }
    return best;
}

kelly::AllocationResult full_kelly(const std::vector<double>& p, const std::vector<double>& o) {
    // kelly_fraction 1.0, exposure cap 1.0 -> raw full-Kelly, directly
    // comparable to the brute-force reference.
    return allocate(p, o, 1.0, 1.0);
}

} // namespace

TEST_CASE("single +EV bet matches the closed-form single-bet Kelly", "[kelly]") {
    // p=0.6, o=2.0: f = (p*o-1)/(o-1) = 0.2/1 = 0.2.
    std::vector<double> p = {0.6, 0.4}, o = {2.0, 1.0 / 0.4 - 0.001};
    // Make outcome 1 clearly -EV so only outcome 0 is bet.
    o = {2.0, 1.5};
    auto r = full_kelly(p, o);
    REQUIRE(r.has_bet);
    REQUIRE(r.bets == 1);
    REQUIRE_THAT(r.stake[0], WithinAbs(0.2, 1e-9));
    REQUIRE(r.stake[1] == 0.0);
}

TEST_CASE("a purely -EV market produces no bet", "[kelly]") {
    std::vector<double> p = {0.3, 0.3, 0.4};
    std::vector<double> o = {1.5, 1.5, 1.2}; // every p*o < 1
    auto r = full_kelly(p, o);
    REQUIRE_FALSE(r.has_bet);
    REQUIRE(r.total_stake == 0.0);
    for (int i = 0; i < 3; ++i) REQUIRE(r.stake[i] == 0.0);
}

TEST_CASE("an arbitrage stakes the whole bankroll with zero reserve", "[kelly]") {
    // Booksum 2/2.1 < 1: a genuine arb. Optimal is to stake everything,
    // reserve rate 0.
    std::vector<double> p = {0.5, 0.5}, o = {2.1, 2.1};
    auto r = full_kelly(p, o);
    REQUIRE(r.has_bet);
    REQUIRE(r.bets == 2);
    REQUIRE_THAT(r.reserve_rate, WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(r.total_stake, WithinAbs(1.0, 1e-12));
}

TEST_CASE("fractional Kelly scales every stake by the fraction", "[kelly]") {
    std::vector<double> p = {0.6}, o = {2.0};
    auto full = allocate(p, o, 1.0, 1.0);
    auto quarter = allocate(p, o, 0.25, 1.0);
    REQUIRE_THAT(quarter.stake[0], WithinAbs(full.stake[0] * 0.25, 1e-12));
    REQUIRE_THAT(quarter.total_stake, WithinAbs(full.total_stake * 0.25, 1e-12));
}

TEST_CASE("exposure cap scales stakes down and preserves their ratio", "[kelly]") {
    // Two staked outcomes; cap total below the full-Kelly total.
    std::vector<double> p = {0.55, 0.45}, o = {2.1, 2.4};
    auto uncapped = allocate(p, o, 1.0, 1.0);
    REQUIRE(uncapped.has_bet);
    const double cap = uncapped.total_stake * 0.5;
    auto capped = allocate(p, o, 1.0, cap);
    REQUIRE(capped.capped);
    REQUIRE_THAT(capped.total_stake, WithinAbs(cap, 1e-12));
    // Ratio between the two stakes must be unchanged by the cap.
    REQUIRE_THAT(capped.stake[0] / capped.stake[1],
                 WithinAbs(uncapped.stake[0] / uncapped.stake[1], 1e-12));
}

TEST_CASE("invalid inputs never bet and never throw", "[kelly]") {
    std::vector<double> ok_p = {0.6, 0.4}, ok_o = {2.0, 3.0};
    // Mismatched lengths.
    std::vector<double> short_o = {2.0};
    REQUIRE_FALSE(allocate(ok_p, short_o, 0.25, 1.0).has_bet);
    // Odds <= 1.
    std::vector<double> bad_o = {1.0, 3.0};
    REQUIRE_FALSE(allocate(ok_p, bad_o, 0.25, 1.0).has_bet);
    // Non-finite.
    std::vector<double> nan_o = {std::numeric_limits<double>::quiet_NaN(), 3.0};
    REQUIRE_FALSE(allocate(ok_p, nan_o, 0.25, 1.0).has_bet);
    // Non-positive kelly fraction / exposure.
    REQUIRE_FALSE(allocate(ok_p, ok_o, 0.0, 1.0).has_bet);
    REQUIRE_FALSE(allocate(ok_p, ok_o, 0.25, 0.0).has_bet);
    // Too many outcomes.
    std::vector<double> big_p(5, 0.2), big_o(5, 6.0);
    REQUIRE_FALSE(allocate(big_p, big_o, 0.25, 1.0).has_bet);
}

TEST_CASE("greedy allocation matches independent brute-force search", "[kelly][property]") {
    // A spread of markets: clear favourites, mixed edges, partial +EV sets,
    // and all-negative. The greedy selection must agree with exhaustive subset
    // search on both the stakes and the reserve rate everywhere.
    const std::vector<std::pair<std::vector<double>, std::vector<double>>> cases = {
        {{0.6, 0.4}, {2.0, 1.5}},
        {{0.5, 0.3, 0.2}, {2.5, 2.5, 2.5}},
        {{0.5, 0.5}, {2.1, 2.1}},
        {{0.45, 0.30, 0.25}, {2.4, 3.6, 4.2}},
        {{0.7, 0.2, 0.1}, {1.3, 6.0, 12.0}},
        {{0.33, 0.33, 0.34}, {3.1, 3.1, 3.1}},
        {{0.25, 0.25, 0.25, 0.25}, {4.2, 4.2, 4.2, 4.2}},
        {{0.9, 0.1}, {1.05, 20.0}},
    };

    for (const auto& [p, o] : cases) {
        auto ref = brute_force(p, o);
        auto got = full_kelly(p, o);
        REQUIRE(got.has_bet == ref.bet);
        if (!ref.bet) continue;
        REQUIRE_THAT(got.reserve_rate, WithinAbs(ref.reserve, 1e-9));
        for (size_t i = 0; i < p.size(); ++i)
            REQUIRE_THAT(got.stake[i], WithinAbs(ref.stake[i], 1e-9));
    }
}
