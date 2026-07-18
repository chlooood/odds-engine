#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include "LatentMarket.hpp"

struct BookConfig {
    uint16_t book_id;
    int      lag_ticks;     // how stale this book's view is
    double   overround;     // vig, e.g. 0.05 = 5%
    double   noise_sigma;   // per-book pricing noise in logit space
    double   skew;          // >1 shades longshots harder (favorite-longshot bias)
};

// Turns latent truth into quoted decimal odds for one book, applying
// lag, idiosyncratic noise, and a skewed vig.
class Bookmaker {
public:
    static constexpr int kOutcomes = LatentMarket::kOutcomes;
    static constexpr int kMaxLag = 512;
    static constexpr uint32_t kMaxOddsMilli = 1000000; // cap at 1000.000

    Bookmaker(BookConfig cfg, uint64_t seed)
        : cfg_(cfg), rng_(seed), normal_(0.0, 1.0) {}

    // Feed the current latent state; call once per tick before quote().
    void observe(const std::array<double, kOutcomes>& latent) {
        history_[write_idx_ % kMaxLag] = latent;
        ++write_idx_;
    }

    // Produce this book's quoted odds (milli) from its lagged view.
    std::array<uint32_t, kOutcomes> quote() {
        const auto& lagged = lagged_view();

        // 1. per-book noise in logit space
        std::array<double, kOutcomes> logits{};
        for (int i = 0; i < kOutcomes; ++i) {
            logits[i] = std::log(lagged[i]) + cfg_.noise_sigma * normal_(rng_);
        }
        std::array<double, kOutcomes> p = softmax(logits);

        // 2. skewed vig: raise to power < 1 to shade longshots harder,
        //    then rescale so implied probs sum to (1 + overround)
        std::array<double, kOutcomes> shaded{};
        double sum = 0.0;
        for (int i = 0; i < kOutcomes; ++i) {
            shaded[i] = std::pow(p[i], 1.0 / cfg_.skew);
            sum += shaded[i];
        }
        const double target = 1.0 + cfg_.overround;

        // 3. convert to decimal odds, quantized to milli-odds
        std::array<uint32_t, kOutcomes> odds{};
        for (int i = 0; i < kOutcomes; ++i) {
            double implied = (shaded[i] / sum) * target;
            double decimal = 1.0 / implied;
            double milli = decimal * 1000.0;
            if (milli > kMaxOddsMilli) milli = kMaxOddsMilli;
            if (milli < 1001.0) milli = 1001.0; // odds can never be <= 1.0
            odds[i] = static_cast<uint32_t>(milli + 0.5);
        }
        return odds;
    }

    uint16_t book_id() const { return cfg_.book_id; }

private:
    const std::array<double, kOutcomes>& lagged_view() const {
        // if not enough history yet, use the oldest we have
        uint64_t available = write_idx_;
        uint64_t back = static_cast<uint64_t>(cfg_.lag_ticks);
        if (back >= available) back = available - 1;
        uint64_t idx = (write_idx_ - 1 - back) % kMaxLag;
        return history_[idx];
    }

    static std::array<double, kOutcomes> softmax(const std::array<double, kOutcomes>& logits) {
        double max_l = logits[0];
        for (int i = 1; i < kOutcomes; ++i) if (logits[i] > max_l) max_l = logits[i];
        std::array<double, kOutcomes> exps{};
        double sum = 0.0;
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
