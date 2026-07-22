#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

// Recovering a bookmaker's implied beliefs from its quoted prices.
//
// A book quotes odds whose implied probabilities sum to more than one. The
// excess is its margin. Removing it requires deciding WHERE to take the
// probability mass from, and the three methods here answer that differently:
//
//   multiplicative  every outcome surrenders the same FRACTION of its implied
//                   probability. Exact inverse of proportional margin.
//   power           solve for exponent k with sum(q^k) = 1. Strips more from
//                   longshots than favourites.
//   shin            margin arises from the book protecting itself against a
//                   proportion z of insider traders. Different economic model,
//                   different functional form.
//
// VALIDATION CAVEAT: the power method's functional form (raise to an exponent)
// is the same family as sim/Bookmaker.hpp's VigModel::Power shading. Any
// accuracy comparison run only against power-shaded synthetic data overstates
// the power method's advantage. Run the comparison under both vig models
// before drawing a conclusion. See docs devig-methods.
namespace devig {

inline constexpr int kMaxOutcomes = 4;

using ProbVector = std::array<double, kMaxOutcomes>;

enum class Status : uint8_t {
    Ok,                 // computation succeeded
    NoMarginToRemove,   // overround <= 0; probabilities returned UNCHANGED
    InvalidInput,       // empty, too many outcomes, or any odd <= 1.0 / non-finite
    NotConverged,       // solver failed to reach tolerance
};

struct DevigResult {
    ProbVector prob{};
    double param = 1.0;     // power: exponent k. shin: insider fraction z. else unused.
    int iterations = 0;
    uint8_t count = 0;      // number of populated entries in prob
    Status status = Status::InvalidInput;
    bool normalized = false; // true only when prob is guaranteed to sum to 1

    bool usable() const { return status == Status::Ok; }

    // Consensus must gate on this, not on usable() alone: a NoMarginToRemove
    // result is a legitimate computation whose vector does NOT sum to 1, and
    // averaging it against normalized vectors silently corrupts the mean.
    bool usable_as_distribution() const { return status == Status::Ok && normalized; }
};

// Decimal odds must exceed 1.0. Odds of exactly 1.0 imply certainty (q = 1,
// making sum(q^k) > 1 for every k, so no root exists) and odds below 1.0 imply
// probability above 1. Checked rather than assumed: session files can be
// corrupt and live feeds can be wrong.
inline bool valid_odds(std::span<const double> odds) {
    if (odds.empty() || odds.size() > static_cast<size_t>(kMaxOutcomes)) return false;
    for (double o : odds) {
        if (!std::isfinite(o) || !(o > 1.0)) return false;
    }
    return true;
}

inline double booksum(std::span<const double> odds) {
    double sum = 0.0;
    for (double o : odds) sum += 1.0 / o;
    return sum;
}

inline double overround(std::span<const double> odds) {
    return booksum(odds) - 1.0;
}

// Raw implied probabilities. Deliberately NOT normalized: the vector sums to
// 1 + overround by construction, which is the whole point of computing it.
inline DevigResult implied(std::span<const double> odds) {
    DevigResult out{};
    if (!valid_odds(odds)) return out;
    out.count = static_cast<uint8_t>(odds.size());
    for (size_t i = 0; i < odds.size(); ++i) out.prob[i] = 1.0 / odds[i];
    out.status = Status::Ok;
    out.normalized = false;
    return out;
}

namespace detail {
// Shared by every method: when the book has no positive margin there is
// nothing to remove, and normalizing anyway would invent margin that is not
// there. Returns raw implied probabilities flagged as non-normalized.
inline DevigResult no_margin(std::span<const double> odds) {
    DevigResult out{};
    out.count = static_cast<uint8_t>(odds.size());
    for (size_t i = 0; i < odds.size(); ++i) out.prob[i] = 1.0 / odds[i];
    out.param = 1.0;
    out.status = Status::NoMarginToRemove;
    out.normalized = false;
    return out;
}
} // namespace detail

inline DevigResult multiplicative(std::span<const double> odds) {
    DevigResult out{};
    if (!valid_odds(odds)) return out;

    const double B = booksum(odds);
    if (B <= 1.0) return detail::no_margin(odds);

    out.count = static_cast<uint8_t>(odds.size());
    for (size_t i = 0; i < odds.size(); ++i) out.prob[i] = (1.0 / odds[i]) / B;
    out.status = Status::Ok;
    out.normalized = true;
    return out;
}

// Solve for k satisfying sum((1/o_i)^k) == 1, then p_i = (1/o_i)^k.
//
// Every q_i lies strictly in (0,1), so raising to k > 1 shrinks small values
// proportionally harder than large ones: more margin comes off longshots than
// favourites. That is the favourite-longshot correction.
//
// f(k) = sum(q_i^k) - 1 is strictly decreasing in k, since every derivative
// term q_i^k * ln(q_i) is negative. The root is unique and bracketing is
// unconditionally safe. Newton is used for speed but every step is validated
// against the bracket and replaced by bisection if it would escape: on a
// function this flat near the root, unguarded Newton overshoots badly when the
// overround is small.
inline DevigResult power(std::span<const double> odds,
                         double tolerance = 1e-13,
                         int max_iterations = 100) {
    DevigResult out{};
    if (!valid_odds(odds)) return out;

    const size_t n = odds.size();
    out.count = static_cast<uint8_t>(n);

    std::array<double, kMaxOutcomes> log_q{};
    for (size_t i = 0; i < n; ++i) log_q[i] = std::log(1.0 / odds[i]);

    auto f = [&](double k) {
        double sum = 0.0;
        for (size_t i = 0; i < n; ++i) sum += std::exp(k * log_q[i]);
        return sum - 1.0;
    };
    auto df = [&](double k) {
        double sum = 0.0;
        for (size_t i = 0; i < n; ++i) sum += log_q[i] * std::exp(k * log_q[i]);
        return sum;
    };

    if (f(1.0) <= 0.0) return detail::no_margin(odds);

    double lo = 1.0;
    double hi = 2.0;
    while (f(hi) > 0.0 && hi < 1024.0) hi *= 2.0;
    if (f(hi) > 0.0) {
        out.status = Status::NotConverged;
        return out;
    }

    double k = lo + (hi - lo) * 0.5;
    out.status = Status::NotConverged;

    for (int it = 1; it <= max_iterations; ++it) {
        const double fk = f(k);
        out.iterations = it;

        if (std::abs(fk) < tolerance) {
            out.status = Status::Ok;
            out.normalized = true;
            break;
        }

        if (fk > 0.0) lo = k; else hi = k;

        const double dfk = df(k);
        double next = (dfk != 0.0) ? k - fk / dfk : k;
        if (!std::isfinite(next) || !(next > lo && next < hi)) {
            next = lo + (hi - lo) * 0.5;
        }
        k = next;
    }

    for (size_t i = 0; i < n; ++i) out.prob[i] = std::exp(k * log_q[i]);
    out.param = k;
    return out;
}

// Shin (1993): the book sets prices to protect itself against a proportion z
// of traders who know the outcome. Given the booksum B and observed implied
// probabilities q, the true probabilities satisfy
//
//   p_i = [ sqrt(z^2 + 4(1-z) q_i^2 / B) - z ] / (2(1-z))
//
// and z is fixed by requiring sum(p_i) = 1.
//
// This is a genuinely different functional form from the power method, not a
// reparametrisation of it, which is why it is here: it gives the validation
// experiment a third estimate that does not share a family with the
// simulator's power shading.
//
// Bracketing: at z = 0 the expression reduces to q_i / sqrt(B), so
// g(0) = sqrt(B) - 1 > 0 whenever B > 1. As z approaches 1 the limit is
// q_i^2 / B, and sum(q_i^2)/B <= max(q_i) < 1, so g(1-) < 0. The root is
// therefore bracketed on [0, 1) for every valid input, and plain bisection is
// used because the derivative is awkward and the solve is not on a hot path.
inline DevigResult shin(std::span<const double> odds,
                        double tolerance = 1e-15,
                        int max_iterations = 200) {
    DevigResult out{};
    if (!valid_odds(odds)) return out;

    const size_t n = odds.size();
    out.count = static_cast<uint8_t>(n);

    const double B = booksum(odds);
    if (B <= 1.0) return detail::no_margin(odds);

    std::array<double, kMaxOutcomes> q{};
    for (size_t i = 0; i < n; ++i) q[i] = 1.0 / odds[i];

    auto prob_at = [&](double z, ProbVector& p) {
        const double denom = 2.0 * (1.0 - z);
        for (size_t i = 0; i < n; ++i) {
            const double inner = z * z + 4.0 * (1.0 - z) * q[i] * q[i] / B;
            p[i] = (std::sqrt(inner) - z) / denom;
        }
    };
    auto g = [&](double z) {
        ProbVector p{};
        prob_at(z, p);
        double sum = 0.0;
        for (size_t i = 0; i < n; ++i) sum += p[i];
        return sum - 1.0;
    };

    double lo = 0.0;
    double hi = 1.0 - 1e-9;   // z = 1 is a removable singularity; stay off it
    if (g(hi) > 0.0) {
        out.status = Status::NotConverged;
        return out;
    }

    double z = 0.5 * (lo + hi);
    out.status = Status::NotConverged;

    for (int it = 1; it <= max_iterations; ++it) {
        const double gz = g(z);
        out.iterations = it;

        if (std::abs(gz) < tolerance) {
            out.status = Status::Ok;
            out.normalized = true;
            break;
        }
        if (gz > 0.0) lo = z; else hi = z;
        z = 0.5 * (lo + hi);
    }

    prob_at(z, out.prob);
    out.param = z;
    return out;
}

} // namespace devig
