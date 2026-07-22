#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <array>
#include <cmath>
#include <limits>
#include "../engine/Devig.hpp"

using Catch::Matchers::WithinAbs;
using namespace devig;

// Reference values throughout: analytics/devig_reference.py, solved with
// scipy.optimize.brentq. Brent is a different algorithm from the
// Newton-with-bisection hybrid in power() and the plain bisection in shin(),
// so agreement is evidence rather than circular confirmation.

namespace {

double sum_of(const DevigResult& r) {
    double s = 0.0;
    for (uint8_t i = 0; i < r.count; ++i) s += r.prob[i];
    return s;
}

// Checks the DEFINING EQUATION rather than a stored value: if power() returned
// k, then sum(q_i^k) must be 1. Independent of any reference implementation.
double power_residual(std::span<const double> odds, double k) {
    double sum = 0.0;
    for (double o : odds) sum += std::pow(1.0 / o, k);
    return sum - 1.0;
}

// Same idea for Shin: at the returned z, the reconstructed probabilities must
// sum to 1 when recomputed from scratch.
double shin_residual(std::span<const double> odds, double z) {
    double B = 0.0;
    for (double o : odds) B += 1.0 / o;
    double sum = 0.0;
    for (double o : odds) {
        const double q = 1.0 / o;
        sum += (std::sqrt(z * z + 4.0 * (1.0 - z) * q * q / B) - z) / (2.0 * (1.0 - z));
    }
    return sum - 1.0;
}

constexpr std::array<double, 3> kTight   = {2.15, 3.40, 4.10};  // overround 0.0031363652
constexpr std::array<double, 3> kFav     = {1.50, 4.00, 7.00};  // overround 0.0595238095
constexpr std::array<double, 3> kHeavy   = {1.20, 8.00, 15.00}; // overround 0.0250000000
constexpr std::array<double, 3> kExtreme = {1.04, 21.00, 34.00};// overround 0.0385692739
constexpr std::array<double, 2> kSym     = {1.91, 1.91};        // overround 0.0471204188
constexpr std::array<double, 3> kArb     = {8.00, 9.00, 10.00}; // overround -0.6638888889

} // namespace

// ── input validation ─────────────────────────────────────────────────────────

TEST_CASE("odds of exactly 1.0 are rejected by every method", "[devig][validation]") {
    // q = 1.0 makes sum(q^k) > 1 for every k, so no root exists. Without the
    // guard the solver burned its full iteration budget and returned garbage
    // with no indication anything was wrong.
    constexpr std::array<double, 3> bad = {1.0, 5.0, 6.0};
    REQUIRE(multiplicative(bad).status == Status::InvalidInput);
    REQUIRE(power(bad).status          == Status::InvalidInput);
    REQUIRE(shin(bad).status           == Status::InvalidInput);
    REQUIRE(implied(bad).status        == Status::InvalidInput);
    REQUIRE_FALSE(power(bad).usable());
    REQUIRE_FALSE(power(bad).usable_as_distribution());
}

TEST_CASE("odds below 1.0 are rejected", "[devig][validation]") {
    constexpr std::array<double, 3> bad = {0.5, 5.0, 6.0};
    REQUIRE(power(bad).status          == Status::InvalidInput);
    REQUIRE(multiplicative(bad).status == Status::InvalidInput);
    REQUIRE(shin(bad).status           == Status::InvalidInput);
}

TEST_CASE("non-finite odds are rejected", "[devig][validation]") {
    const std::array<double, 3> inf_case = {2.0, std::numeric_limits<double>::infinity(), 6.0};
    const std::array<double, 3> nan_case = {2.0, std::numeric_limits<double>::quiet_NaN(), 6.0};
    REQUIRE(power(inf_case).status == Status::InvalidInput);
    REQUIRE(power(nan_case).status == Status::InvalidInput);
    REQUIRE(shin(nan_case).status  == Status::InvalidInput);
}

TEST_CASE("more outcomes than kMaxOutcomes is rejected", "[devig][validation]") {
    // ProbVector is fixed at kMaxOutcomes. Before the guard, a five-outcome
    // market wrote past the end of the array.
    constexpr std::array<double, 5> too_many = {2.0, 5.0, 6.0, 8.0, 9.0};
    REQUIRE(power(too_many).status          == Status::InvalidInput);
    REQUIRE(multiplicative(too_many).status == Status::InvalidInput);
    REQUIRE(shin(too_many).status           == Status::InvalidInput);
}

TEST_CASE("empty input is rejected", "[devig][validation]") {
    constexpr std::array<double, 0> empty{};
    REQUIRE(power(empty).status          == Status::InvalidInput);
    REQUIRE(multiplicative(empty).status == Status::InvalidInput);
    REQUIRE(shin(empty).status           == Status::InvalidInput);
}

// ── implied and overround ────────────────────────────────────────────────────

TEST_CASE("implied probability is the reciprocal of the odds", "[devig][implied]") {
    constexpr std::array<double, 3> odds = {2.00, 4.00, 5.00};
    const auto r = implied(odds);
    REQUIRE(r.usable());
    REQUIRE(r.count == 3);
    REQUIRE_THAT(r.prob[0], WithinAbs(0.50, 1e-15));
    REQUIRE_THAT(r.prob[1], WithinAbs(0.25, 1e-15));
    REQUIRE_THAT(r.prob[2], WithinAbs(0.20, 1e-15));
}

TEST_CASE("implied is deliberately not a distribution", "[devig][implied]") {
    // implied() succeeds but its output sums to 1 + overround by design.
    // Consensus must therefore gate on usable_as_distribution(), not usable().
    const auto r = implied(kFav);
    REQUIRE(r.usable());
    REQUIRE_FALSE(r.normalized);
    REQUIRE_FALSE(r.usable_as_distribution());
    REQUIRE_THAT(sum_of(r), WithinAbs(1.0595238095, 1e-9));
}

TEST_CASE("overround is the excess of the booksum above 1", "[devig][overround]") {
    REQUIRE_THAT(overround(kFav),   WithinAbs(0.0595238095, 1e-9));
    REQUIRE_THAT(overround(kTight), WithinAbs(0.0031363652, 1e-9));
    REQUIRE_THAT(overround(kArb),   WithinAbs(-0.6638888889, 1e-9));
    REQUIRE_THAT(booksum(kFav),     WithinAbs(1.0595238095, 1e-9));
}

// ── multiplicative ───────────────────────────────────────────────────────────

TEST_CASE("multiplicative output is a distribution", "[devig][mult]") {
    for (const auto& r : {multiplicative(kTight), multiplicative(kFav),
                          multiplicative(kHeavy), multiplicative(kExtreme)}) {
        REQUIRE(r.usable_as_distribution());
        REQUIRE_THAT(sum_of(r), WithinAbs(1.0, 1e-14));
    }
    const auto s = multiplicative(kSym);
    REQUIRE(s.usable_as_distribution());
    REQUIRE_THAT(sum_of(s), WithinAbs(1.0, 1e-14));
}

TEST_CASE("multiplicative known values, tight market", "[devig][mult]") {
    const auto r = multiplicative(kTight);
    REQUIRE_THAT(r.prob[0], WithinAbs(0.463662065525, 1e-9));
    REQUIRE_THAT(r.prob[1], WithinAbs(0.293198070846, 1e-9));
    REQUIRE_THAT(r.prob[2], WithinAbs(0.243139863629, 1e-9));
}

TEST_CASE("multiplicative known values, clear favourite", "[devig][mult]") {
    const auto r = multiplicative(kFav);
    REQUIRE_THAT(r.prob[0], WithinAbs(0.629213483146, 1e-9));
    REQUIRE_THAT(r.prob[1], WithinAbs(0.235955056180, 1e-9));
    REQUIRE_THAT(r.prob[2], WithinAbs(0.134831460674, 1e-9));
}

// ── power ────────────────────────────────────────────────────────────────────

TEST_CASE("power satisfies its defining equation at the returned k", "[devig][power]") {
    REQUIRE_THAT(power_residual(kTight,   power(kTight).param),   WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(power_residual(kFav,     power(kFav).param),     WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(power_residual(kHeavy,   power(kHeavy).param),   WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(power_residual(kExtreme, power(kExtreme).param), WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(power_residual(kSym,     power(kSym).param),     WithinAbs(0.0, 1e-12));
}

TEST_CASE("power output is a distribution", "[devig][power]") {
    for (const auto& r : {power(kTight), power(kFav), power(kHeavy), power(kExtreme)}) {
        REQUIRE(r.usable_as_distribution());
        REQUIRE_THAT(sum_of(r), WithinAbs(1.0, 1e-12));
    }
}

TEST_CASE("power known values, tight market", "[devig][power]") {
    const auto r = power(kTight);
    REQUIRE(r.usable_as_distribution());
    REQUIRE_THAT(r.param,   WithinAbs(1.002963490641718, 1e-9));
    REQUIRE_THAT(r.prob[0], WithinAbs(0.464062378706, 1e-9));
    REQUIRE_THAT(r.prob[1], WithinAbs(0.293052918030, 1e-9));
    REQUIRE_THAT(r.prob[2], WithinAbs(0.242884703264, 1e-9));
}

TEST_CASE("power known values, clear favourite", "[devig][power]") {
    const auto r = power(kFav);
    REQUIRE_THAT(r.param,   WithinAbs(1.069458513463318, 1e-9));
    REQUIRE_THAT(r.prob[0], WithinAbs(0.648153251231, 1e-9));
    REQUIRE_THAT(r.prob[1], WithinAbs(0.227050162289, 1e-9));
    REQUIRE_THAT(r.prob[2], WithinAbs(0.124796586480, 1e-9));
}

TEST_CASE("power known values, extreme favourite", "[devig][power]") {
    // Largest correction in the suite: k = 1.168 means the book loaded heavy
    // margin onto the longshots, and power strips proportionally more from them.
    const auto r = power(kExtreme);
    REQUIRE_THAT(r.param,   WithinAbs(1.168223324223036, 1e-9));
    REQUIRE_THAT(r.prob[0], WithinAbs(0.955215268464, 1e-9));
    REQUIRE_THAT(r.prob[1], WithinAbs(0.028533337945, 1e-9));
    REQUIRE_THAT(r.prob[2], WithinAbs(0.016251393590, 1e-9));
}

TEST_CASE("power on a symmetric two-outcome market", "[devig][power]") {
    // No favourite-longshot skew exists, so the correction must be a no-op
    // in output even though k != 1.
    const auto r = power(kSym);
    REQUIRE(r.usable_as_distribution());
    REQUIRE(r.count == 2);
    REQUIRE_THAT(r.prob[0], WithinAbs(0.5, 1e-12));
    REQUIRE_THAT(r.prob[1], WithinAbs(0.5, 1e-12));
}

// ── shin ─────────────────────────────────────────────────────────────────────

TEST_CASE("shin satisfies its defining equation at the returned z", "[devig][shin]") {
    REQUIRE_THAT(shin_residual(kTight,   shin(kTight).param),   WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(shin_residual(kFav,     shin(kFav).param),     WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(shin_residual(kHeavy,   shin(kHeavy).param),   WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(shin_residual(kExtreme, shin(kExtreme).param), WithinAbs(0.0, 1e-12));
}

TEST_CASE("shin output is a distribution", "[devig][shin]") {
    for (const auto& r : {shin(kTight), shin(kFav), shin(kHeavy), shin(kExtreme)}) {
        REQUIRE(r.usable_as_distribution());
        REQUIRE_THAT(sum_of(r), WithinAbs(1.0, 1e-12));
    }
}

TEST_CASE("shin insider fraction stays in its valid range", "[devig][shin]") {
    // z is a proportion of informed traders: it must lie in [0, 1).
    for (const auto& r : {shin(kTight), shin(kFav), shin(kHeavy), shin(kExtreme), shin(kSym)}) {
        REQUIRE(r.param >= 0.0);
        REQUIRE(r.param < 1.0);
    }
}

TEST_CASE("shin known values, tight market", "[devig][shin]") {
    const auto r = shin(kTight);
    REQUIRE(r.usable_as_distribution());
    REQUIRE_THAT(r.param,   WithinAbs(0.001568392073187, 1e-9));
    REQUIRE_THAT(r.prob[0], WithinAbs(0.463968439493, 1e-9));
    REQUIRE_THAT(r.prob[1], WithinAbs(0.293103676932, 1e-9));
    REQUIRE_THAT(r.prob[2], WithinAbs(0.242927883574, 1e-9));
}

TEST_CASE("shin known values, clear favourite", "[devig][shin]") {
    const auto r = shin(kFav);
    REQUIRE_THAT(r.param,   WithinAbs(0.030260813864828, 1e-9));
    REQUIRE_THAT(r.prob[0], WithinAbs(0.642279562151, 1e-9));
    REQUIRE_THAT(r.prob[1], WithinAbs(0.231526875399, 1e-9));
    REQUIRE_THAT(r.prob[2], WithinAbs(0.126193562449, 1e-9));
}

TEST_CASE("shin known values, extreme favourite", "[devig][shin]") {
    const auto r = shin(kExtreme);
    REQUIRE_THAT(r.param,   WithinAbs(0.022326931793620, 1e-9));
    REQUIRE_THAT(r.prob[0], WithinAbs(0.942877787419, 1e-9));
    REQUIRE_THAT(r.prob[1], WithinAbs(0.037198506383, 1e-9));
    REQUIRE_THAT(r.prob[2], WithinAbs(0.019923706198, 1e-9));
}

TEST_CASE("shin on a symmetric two-outcome market", "[devig][shin]") {
    const auto r = shin(kSym);
    REQUIRE(r.usable_as_distribution());
    REQUIRE_THAT(r.prob[0], WithinAbs(0.5, 1e-12));
    REQUIRE_THAT(r.prob[1], WithinAbs(0.5, 1e-12));
    // Known analytic property: for a symmetric two-outcome book the insider
    // fraction equals the overround exactly.
    REQUIRE_THAT(r.param, WithinAbs(overround(kSym), 1e-9));
}

// ── cross-method properties ──────────────────────────────────────────────────

TEST_CASE("longshot probability is ordered power <= shin <= multiplicative",
          "[devig][property]") {
    // Each method strips a different amount of margin from the longshot.
    // Power is the most aggressive, multiplicative the least, Shin between.
    // If this ordering breaks, one of the corrections is pointing the wrong way.
    for (const auto& odds : {kFav, kHeavy, kExtreme}) {
        const auto m = multiplicative(odds);
        const auto p = power(odds);
        const auto s = shin(odds);
        REQUIRE(p.prob[2] <= s.prob[2]);
        REQUIRE(s.prob[2] <= m.prob[2]);
        // Mass removed from the longshot has to reappear on the favourite.
        REQUIRE(p.prob[0] >= s.prob[0]);
        REQUIRE(s.prob[0] >= m.prob[0]);
    }
}

TEST_CASE("all methods agree on a symmetric market", "[devig][property]") {
    // With no skew to correct, the three methods must be indistinguishable.
    const auto m = multiplicative(kSym);
    const auto p = power(kSym);
    const auto s = shin(kSym);
    REQUIRE_THAT(m.prob[0], WithinAbs(p.prob[0], 1e-12));
    REQUIRE_THAT(p.prob[0], WithinAbs(s.prob[0], 1e-12));
}

TEST_CASE("negative overround yields NoMarginToRemove from every method",
          "[devig][property]") {
    // Booksum 0.336: a within-book arbitrage. No method may normalise this,
    // because doing so invents margin that is not there. The vector is
    // returned untouched and flagged non-normalized so consensus excludes it.
    for (const auto& r : {multiplicative(kArb), power(kArb), shin(kArb)}) {
        REQUIRE(r.status == Status::NoMarginToRemove);
        REQUIRE_FALSE(r.normalized);
        REQUIRE_FALSE(r.usable_as_distribution());
        REQUIRE_THAT(r.prob[0], WithinAbs(1.0 / 8.0,  1e-12));
        REQUIRE_THAT(r.prob[1], WithinAbs(1.0 / 9.0,  1e-12));
        REQUIRE_THAT(r.prob[2], WithinAbs(1.0 / 10.0, 1e-12));
        REQUIRE_THAT(sum_of(r), WithinAbs(0.336111111111, 1e-9));
    }
}
