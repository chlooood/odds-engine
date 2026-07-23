#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

// Kelly staking for the mutually-exclusive-outcome case: given a consensus
// fair probability per outcome and the best available decimal odds per
// outcome, choose the bankroll fraction to stake on each so as to maximise
// expected log wealth.
//
// This is the exact allocator, not a per-outcome single-bet approximation.
// The single-bet formula f = (p*o - 1)/(o - 1) is correct only when you bet on
// one outcome in isolation; when several outcomes of the same event are
// simultaneously +EV, staking each at its single-bet fraction overstakes,
// because a win on any one returns the stakes lost on the others. The exact
// solution (Smoczynski & Tomkins 2010) selects a betting set and a common
// reserve rate R such that each staked outcome gets f_i = p_i - R/o_i.
//
// Kept as a free function over spans, matching Devig.hpp and Consensus.hpp
// rather than the class sketch in the original design doc: the inputs are a
// fixed, small, known set (two vectors and two scalars) and there is no state
// to carry between calls, so a class would add ceremony without behaviour.
namespace kelly {

inline constexpr int kMaxOutcomes = 4;

struct AllocationResult {
    // Bankroll fraction to stake on each outcome, AFTER fractional-Kelly
    // scaling and the exposure cap. Zero for outcomes not in the betting set.
    std::array<double, kMaxOutcomes> stake{};
    uint8_t count = 0;
    double total_stake = 0.0;   // sum of stake[], <= max_exposure
    double reserve_rate = 1.0;  // full-Kelly reserve R for the chosen set
    size_t bets = 0;            // number of outcomes with a positive stake
    bool has_bet = false;       // false when no outcome is +EV
    bool capped = false;        // true when the exposure cap bound the stakes
};

// fair:  consensus probabilities (need not be exactly normalised; the maths
//        does not require sum == 1, though in practice consensus supplies it).
// odds:  best available decimal odds per outcome, each > 1.0.
// kelly_fraction: fractional-Kelly multiplier, e.g. 0.25 for quarter-Kelly.
//        Full Kelly is optimal for log wealth but has punishing variance;
//        fractional Kelly trades a little growth for a lot less drawdown.
// max_exposure: hard cap on total staked fraction. Applied after fractional
//        Kelly by scaling all stakes down proportionally if they exceed it,
//        which preserves the relative allocation across outcomes.
//
// Returns has_bet == false (all stakes zero) on invalid input or when no
// outcome has positive edge. Never throws.
inline AllocationResult allocate(std::span<const double> fair,
                                 std::span<const double> odds,
                                 double kelly_fraction,
                                 double max_exposure) {
    AllocationResult out{};

    const size_t n = fair.size();
    if (n == 0 || n > static_cast<size_t>(kMaxOutcomes) || odds.size() != n) return out;
    if (!(kelly_fraction > 0.0) || !(max_exposure > 0.0)) return out;
    for (size_t i = 0; i < n; ++i) {
        if (!std::isfinite(fair[i]) || !std::isfinite(odds[i])) return out;
        if (fair[i] < 0.0 || !(odds[i] > 1.0)) return out;
    }
    out.count = static_cast<uint8_t>(n);

    // Order outcomes by expected revenue rate p*o, descending. The optimal
    // betting set is always a prefix of this order: if a lower-edge outcome is
    // worth a stake, every higher-edge one is too.
    std::array<int, kMaxOutcomes> order{};
    for (size_t i = 0; i < n; ++i) order[i] = static_cast<int>(i);
    for (size_t i = 1; i < n; ++i) {           // insertion sort, n <= 4
        const int key = order[i];
        const double kv = fair[key] * odds[key];
        size_t j = i;
        while (j > 0 && fair[order[j - 1]] * odds[order[j - 1]] < kv) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = key;
    }

    // Largest prefix length k for which the set is feasible (its implied
    // probabilities sum below 1, so a reserve exists) and its weakest member
    // still beats the reserve rate. Scanning from k = n downward and taking
    // the first hit yields the largest such k.
    int best_k = 0;
    double best_R = 1.0;
    for (int k = static_cast<int>(n); k >= 1; --k) {
        double P = 0.0, B = 0.0;
        for (int t = 0; t < k; ++t) {
            P += fair[order[t]];
            B += 1.0 / odds[order[t]];
        }
        const double denom = 1.0 - B;
        if (denom <= 0.0) continue;                 // set stakes too much; shrink
        const double R = (1.0 - P) / denom;
        const int weakest = order[k - 1];
        if (fair[weakest] * odds[weakest] > R) {    // weakest member still +edge vs reserve
            best_k = k;
            best_R = R;
            break;
        }
    }

    if (best_k == 0) return out;                    // nothing is +EV: stake nothing
    out.reserve_rate = best_R;

    // Full-Kelly stakes for the chosen set: f_i = p_i - R/o_i, each strictly
    // positive by the selection above. These sum to 1 - R.
    double full_total = 0.0;
    for (int t = 0; t < best_k; ++t) {
        const int i = order[t];
        double f = fair[i] - best_R / odds[i];
        if (f < 0.0) f = 0.0;                       // numerical floor
        out.stake[i] = f;
        full_total += f;
        ++out.bets;
    }

    for (size_t i = 0; i < n; ++i) out.stake[i] *= kelly_fraction;
    double total = full_total * kelly_fraction;

    if (total > max_exposure) {
        const double scale = max_exposure / total;
        for (size_t i = 0; i < n; ++i) out.stake[i] *= scale;
        total = max_exposure;
        out.capped = true;
    }

    out.total_stake = total;
    out.has_bet = total > 0.0;
    return out;
}

} // namespace kelly
