#include <catch2/catch_test_macros.hpp>
#include <set>
#include "../sim/Seeding.hpp"

TEST_CASE("derive_seed is a pure function", "[seed]") {
    REQUIRE(derive_seed(42, 1, 0) == derive_seed(42, 1, 0));
    REQUIRE(derive_seed(42, 1, 0) != derive_seed(43, 1, 0));
    REQUIRE(derive_seed(42, 1, 0) != derive_seed(42, 2, 0));
    REQUIRE(derive_seed(42, 1, 0) != derive_seed(42, 1, 1));
}

TEST_CASE("no seed collisions across realistic roster", "[seed]") {
    std::set<uint64_t> seen;
    for (uint64_t domain = 1; domain <= 3; ++domain)
        for (uint64_t i = 0; i < 5000; ++i)
            REQUIRE(seen.insert(derive_seed(42, domain, i)).second);
}
