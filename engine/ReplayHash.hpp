#pragma once
#include <cstdint>
#include <cstring>

// FNV-1a over raw bytes, used only to collapse an entire session's computed
// fair-price trajectory into one digest for the replay-determinism test. Not a
// cryptographic hash and not meant to be: the requirement is "same seed ->
// identical digest, different seed -> different digest", which any decent
// avalanche gives. Chosen over xxhash to avoid a dependency for something this
// small.
//
// Hashing raw double bytes makes the digest platform-dependent in exactly the
// way already documented in docs/open-questions.md Q2 (the RNG distributions
// aren't cross-platform-specified either), so the determinism guarantee is
// "one binary on one machine," which is all the regression test needs.
class ReplayHash {
public:
    void mix_bytes(const void* data, size_t n) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < n; ++i) {
            state_ ^= p[i];
            state_ *= kPrime;
        }
    }

    void mix_u16(uint16_t v)  { mix_bytes(&v, sizeof(v)); }
    void mix_u64(uint64_t v)  { mix_bytes(&v, sizeof(v)); }
    void mix_double(double v) { mix_bytes(&v, sizeof(v)); }

    uint64_t digest() const { return state_; }

private:
    static constexpr uint64_t kOffsetBasis = 14695981039346656037ULL;
    static constexpr uint64_t kPrime       = 1099511628211ULL;
    uint64_t state_ = kOffsetBasis;
};
