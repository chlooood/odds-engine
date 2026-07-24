#pragma once
#include <array>
#include <cmath>
#include <random>
class LatentMarket {
public:
    static constexpr int kOutcomes = 3;
    LatentMarket(uint64_t seed, std::array<double, kOutcomes> initial_probs)
        : rng_(seed), normal_(0.0, 1.0) {
        for (int i = 0; i < kOutcomes; ++i) {
            initial_logits_[i] = std::log(initial_probs[i]);
            logits_[i] = initial_logits_[i];
        }
    }
    std::array<double, kOutcomes> step() {
        constexpr double kDriftSigma = 0.05;
        constexpr double kReversionRate = 0.02;
        constexpr double kJumpProb = 0.005;
        constexpr double kJumpMagnitude = 1.5;
        for (int i = 0; i < kOutcomes; ++i) {
            double reversion = kReversionRate * (initial_logits_[i] - logits_[i]);
            logits_[i] += reversion + kDriftSigma * normal_(rng_);
        }
        std::uniform_real_distribution<double> unif(0.0, 1.0);
        jumped_ = false;
        if (unif(rng_) < kJumpProb) {
            std::uniform_int_distribution<int> pick(0, kOutcomes - 1);
            int winner = pick(rng_);
            logits_[winner] += kJumpMagnitude;
            jumped_ = true;   // read-only signal; does not perturb the RNG stream
        }
        return softmax();
    }
    // True iff the most recent step() applied a jump. Used only to record a
    // ground-truth jump timeline for offline analysis; never read by the engine.
    bool jumped() const { return jumped_; }
private:
    std::array<double, kOutcomes> softmax() const {
        double max_logit = logits_[0];
        for (int i = 1; i < kOutcomes; ++i) if (logits_[i] > max_logit) max_logit = logits_[i];
        std::array<double, kOutcomes> exps{}; double sum = 0.0;
        for (int i = 0; i < kOutcomes; ++i) { exps[i] = std::exp(logits_[i] - max_logit); sum += exps[i]; }
        std::array<double, kOutcomes> probs{};
        for (int i = 0; i < kOutcomes; ++i) probs[i] = exps[i] / sum;
        return probs;
    }
    std::array<double, kOutcomes> initial_logits_{};
    std::array<double, kOutcomes> logits_{};
    std::mt19937_64 rng_;
    std::normal_distribution<double> normal_;
    bool jumped_ = false;
};
