#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include "LatentMarket.hpp"

// How a book converts its belief into a quoted price by adding margin.
//
// Two models are provided because the choice determines which devig method
// looks best. Power shading and the power devig method share a functional
// family, so measuring devig accuracy only against Power-shaded data
// flatters the power method. Additive shading is structurally unrelated to
// every devig method in engine/Devig.hpp, so it is the honest control.
enum class VigModel : uint8_t {
    Power = 0,        // p^(1/skew), renormalised, scaled to 1 + overround
    Additive = 1,     // equal ABSOLUTE margin per outcome: q_i = p_i + overround/n
    Proportional = 2, // equal RELATIVE margin: q_i = p_i * (1 + overround)
};

struct BookConfig {
    uint16_t book_id;
    int      lag_ticks;
    double   overround;
    double   noise_sigma;
    double   skew;
    // Trailing member with a default initializer: existing five-element
    // aggregate initialisation of this struct continues to compile unchanged.
    VigModel vig_model = VigModel::Power;
};

class Bookmaker {
public:
    static constexpr int kOutcomes = LatentMarket::kOutcomes;
    static constexpr int kMaxLag = 512;
    static constexpr uint32_t kMaxOddsMilli = 1000000;
    static constexpr uint32_t kMinOddsMilli = 1001;

    Bookmaker(BookConfig cfg, uint64_t seed)
        : cfg_(cfg), rng_(seed), normal_(0.0, 1.0) {}

    void observe(const std::array<double, kOutcomes>& latent) {
        history_[write_idx_ % kMaxLag] = latent;
        ++write_idx_;
    }

    std::array<uint32_t, kOutcomes> quote() {
        const auto& lagged = lagged_view();

        std::array<double, kOutcomes> logits{};
        for (int i = 0; i < kOutcomes; ++i)
            logits[i] = std::log(lagged[i]) + cfg_.noise_sigma * normal_(rng_);
        const std::array<double, kOutcomes> p = softmax(logits);

        std::array<double, kOutcomes> q{};
        switch (cfg_.vig_model) {
        case VigModel::Power: {
            double sum = 0.0;
            for (int i = 0; i < kOutcomes; ++i) {
                q[i] = std::pow(p[i], 1.0 / cfg_.skew);
                sum += q[i];
            }
            const double target = 1.0 + cfg_.overround;
            for (int i = 0; i < kOutcomes; ++i) q[i] = (q[i] / sum) * target;
            break;
        }
        case VigModel::Proportional: {
            // Multiplicative devig is the exact inverse of this. Included
            // precisely so the validation shows each method winning on the
            // data its own assumption generated, which is what makes the
            // model-mismatched comparisons the informative ones.
            const double scale = 1.0 + cfg_.overround;
            for (int i = 0; i < kOutcomes; ++i) q[i] = p[i] * scale;
            break;
        }
        case VigModel::Additive: {
            // Equal absolute margin per outcome. A flat 2% added to a 5%
            // longshot is a 40% relative markup; added to a 60% favourite it
            // is 3.3%. So this also loads margin onto longshots, but by a
            // mechanism no devig method here inverts exactly.
            const double per = cfg_.overround / static_cast<double>(kOutcomes);
            for (int i = 0; i < kOutcomes; ++i) q[i] = p[i] + per;
            break;
        }
        }

        std::array<uint32_t, kOutcomes> odds{};
        for (int i = 0; i < kOutcomes; ++i) {
            // q_i can exceed 1 under Additive when the latent probability is
            // already near certainty. The clamp below is the guard; the booksum
            // then falls slightly short of 1 + overround, which is a truthful
            // consequence rather than an error.
            double milli = (q[i] > 0.0) ? (1000.0 / q[i]) : static_cast<double>(kMaxOddsMilli);
            if (milli > static_cast<double>(kMaxOddsMilli)) milli = kMaxOddsMilli;
            if (milli < static_cast<double>(kMinOddsMilli)) milli = kMinOddsMilli;
            odds[i] = static_cast<uint32_t>(milli + 0.5);
        }
        return odds;
    }

    uint16_t book_id() const { return cfg_.book_id; }

private:
    const std::array<double, kOutcomes>& lagged_view() const {
        uint64_t available = write_idx_;
        uint64_t back = static_cast<uint64_t>(cfg_.lag_ticks);
        if (back >= available) back = available - 1;
        return history_[(write_idx_ - 1 - back) % kMaxLag];
    }

    static std::array<double, kOutcomes> softmax(const std::array<double, kOutcomes>& logits) {
        double max_l = logits[0];
        for (int i = 1; i < kOutcomes; ++i) if (logits[i] > max_l) max_l = logits[i];
        std::array<double, kOutcomes> exps{}; double sum = 0.0;
        for (int i = 0; i < kOutcomes; ++i) { exps[i] = std::exp(logits[i] - max_l); sum += exps[i]; }
        std::array<double, kOutcomes> p{};
        for (int i = 0; i < kOutcomes; ++i) p[i] = exps[i] / sum;
        return p;
    }

    BookConfig cfg_;
    std::array<std::array<double, kOutcomes>, kMaxLag> history_{};
    uint64_t write_idx_ = 0;
    std::mt19937_64 rng_;
    std::normal_distribution<double> normal_;
};
