#pragma once
#include <cstdint>

// Stateless sub-seed derivation. Sub-seeds must be a pure function of
// (master_seed, entity_id) so that adding an entity does not perturb the
// streams of existing ones — otherwise every roster change silently
// invalidates stored replay baselines.
constexpr uint64_t splitmix64(uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

constexpr uint64_t derive_seed(uint64_t master, uint64_t domain, uint64_t index) noexcept {
    return splitmix64(master ^ splitmix64(domain * 0x1000193ULL + index));
}

namespace seed_domain {
inline constexpr uint64_t kLatentMarket = 1;
inline constexpr uint64_t kBookmaker    = 2;
}
