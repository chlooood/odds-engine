#include <catch2/catch_test_macros.hpp>
#include "../engine/ReplayHash.hpp"

TEST_CASE("empty hash is the FNV offset basis", "[replayhash]") {
    ReplayHash h;
    REQUIRE(h.digest() == 14695981039346656037ULL);
}

TEST_CASE("same input sequence yields same digest", "[replayhash]") {
    ReplayHash a, b;
    for (int i = 0; i < 100; ++i) { a.mix_double(i * 0.01); b.mix_double(i * 0.01); }
    REQUIRE(a.digest() == b.digest());
}

TEST_CASE("a single differing value changes the digest", "[replayhash]") {
    ReplayHash a, b;
    a.mix_double(0.5);
    b.mix_double(0.5000001);
    REQUIRE(a.digest() != b.digest());
}

TEST_CASE("order matters", "[replayhash]") {
    // Mixing the same values in a different order must not collide, or a
    // reordering bug in the engine loop would pass the determinism test.
    ReplayHash a, b;
    a.mix_double(1.0); a.mix_double(2.0);
    b.mix_double(2.0); b.mix_double(1.0);
    REQUIRE(a.digest() != b.digest());
}

TEST_CASE("mixing a market id then probs is stable", "[replayhash]") {
    ReplayHash a, b;
    for (auto* h : {&a, &b}) {
        h->mix_u16(3);
        uint8_t count = 3;
        h->mix_bytes(&count, sizeof(count));
        h->mix_double(0.5); h->mix_double(0.3); h->mix_double(0.2);
    }
    REQUIRE(a.digest() == b.digest());
}
