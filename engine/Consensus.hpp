#pragma once
#include <array>
#include <cstdint>
#include <span>
#include "Devig.hpp"

// Consensus fair probability: a weighted average of each book's devigged
// probabilities, excluding books that are stale or whose devig result wasn't
// usable as a distribution.
//
// Deliberately a free function over a plain input struct, not a class that
// reaches into BookTable and Devig itself, for the same reason Devig.hpp is
// free functions rather than a strategy hierarchy (see
// docs/output-sink-abstraction.md): the set of things consensus needs from a
// book - a probability vector, a weight, and whether it's usable - is small,
// fixed, and known now. Decoupling it from BookTable also means it can be
// tested with hand-built inputs, without a table, a session file, or a devig
// call in the test at all.
namespace consensus {

struct BookInput {
    std::array<double, devig::kMaxOutcomes> prob{};
    uint8_t count = 0;
    double weight = 0.0;
    // False excludes this book from the average entirely: staleness (from
    // BookTable::is_stale), a devig result that wasn't usable_as_distribution,
    // or any other reason the caller has to distrust this book right now.
    // Consensus does not know or care which.
    bool available = false;
};

struct ConsensusResult {
    std::array<double, devig::kMaxOutcomes> prob{};
    uint8_t count = 0;
    // False means no book contributed - either every book was unavailable,
    // or every available book had zero or negative weight. prob is left
    // zeroed in that case, not fabricated from a partial or malformed set.
    bool has_fair_price = false;
    double total_weight = 0.0;
    size_t books_used = 0;
};

inline ConsensusResult fair_price(std::span<const BookInput> books) {
    ConsensusResult out{};

    for (const auto& b : books) {
        if (!b.available || b.weight <= 0.0) continue;

        if (out.books_used == 0) {
            out.count = b.count;
        } else if (b.count != out.count) {
            // A book reporting a different outcome_count for what should be
            // the same market is malformed input, not a modelling choice.
            // Skipping it (rather than averaging mismatched-length vectors,
            // or letting the last writer silently redefine `count`) is the
            // same "don't fabricate" stance as the all-stale case below.
            continue;
        }

        for (int i = 0; i < b.count; ++i) out.prob[i] += b.weight * b.prob[i];
        out.total_weight += b.weight;
        ++out.books_used;
    }

    if (out.total_weight <= 0.0) {
        return ConsensusResult{}; // no fair price: return the zeroed default, not out
    }

    for (int i = 0; i < out.count; ++i) out.prob[i] /= out.total_weight;
    out.has_fair_price = true;
    return out;
}

} // namespace consensus
