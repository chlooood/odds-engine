#pragma once
#include <array>
#include <cmath>
#include <random>

// Simulates the "true" probability distribution over outcomes for one match,
// evolving tick by tick via smooth log-odds drift plus occasional jump events.
// This is the ground truth the engine will later try to recover via devig math.
class LatentMarket {
public:
    static constexpr int kOutcomes = 3; // home / draw / away

    LatentMarket(uint64_t seed, std::array<double, kOutcomes> initial_probs)
        : rng_(seed), normal_(0.0, 1.0) {
        for (int i = 0; i < kOutcomes; ++i) {
            logits_[i] = std::log(initial_probs[i]);
        }
    }

    // Advance the process by one tick, return the new probability vector.
    std::array<double, kOutcomes> step() {
        constexpr double kDriftSigma = 0.05;   // smooth noise magnitude
        constexpr double kJumpProb = 0.005;    // 0.5% chance per tick
        constexpr double kJumpMagnitude = 1.5; // logit shift on a jump

        for (int i = 0; i < kOutcomes; ++i) {
            logits_[i] += kDriftSigma * normal_(rng_);
        }

        std::uniform_real_distribution<double> unif(0.0, 1.0);
        if (unif(rng_) < kJumpProb) {
            std::array<double, kOutcomes> probs_now = softmax();
            std::discrete_distribution<int> pick(probs_now.begin(), probs_now.end());
            int winner = pick(rng_);
            logits_[winner] += kJumpMagnitude;
        }

        return softmax();
    }

private:
    std::array<double, kOutcomes> softmax() const {
        double max_logit = logits_[0];
        for (int i = 1; i < kOutcomes; ++i) {
            if (logits_[i] > max_logit) max_logit = logits_[i];
        }
        std::array<double, kOutcomes> exps{};
        double sum = 0.0;
        for (int i = 0; i < kOutcomes; ++i) {
            exps[i] = std::exp(logits_[i] - max_logit); // subtract max for numerical stability
            sum += exps[i];
        }
        std::array<double, kOutcomes> probs{};
        for (int i = 0; i < kOutcomes; ++i) {
            probs[i] = exps[i] / sum;
        }
        return probs;
    }

    std::array<double, kOutcomes> logits_{};
    std::mt19937_64 rng_;
    std::normal_distribution<double> normal_;
};
