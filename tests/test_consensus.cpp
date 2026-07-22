#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <vector>
#include "../engine/Consensus.hpp"

using Catch::Matchers::WithinAbs;
using consensus::BookInput;
using consensus::fair_price;

namespace {
BookInput make(std::array<double, 3> p, double weight, bool available) {
    BookInput b{};
    b.count = 3;
    b.weight = weight;
    b.available = available;
    for (int i = 0; i < 3; ++i) b.prob[i] = p[i];
    return b;
}
}

TEST_CASE("weighted average of two available books", "[consensus]") {
    // Sharp book (weight 3) says 0.60/0.25/0.15, soft book (weight 1) says
    // 0.50/0.30/0.20. Weighted mean: (3*0.60+1*0.50)/4 = 0.575, etc.
    std::vector<BookInput> books = {
        make({0.60, 0.25, 0.15}, 3.0, true),
        make({0.50, 0.30, 0.20}, 1.0, true),
    };
    auto r = fair_price(books);
    REQUIRE(r.has_fair_price);
    REQUIRE(r.books_used == 2);
    REQUIRE_THAT(r.total_weight, WithinAbs(4.0, 1e-12));
    REQUIRE_THAT(r.prob[0], WithinAbs(0.575, 1e-12));
    REQUIRE_THAT(r.prob[1], WithinAbs(0.2625, 1e-12));
    REQUIRE_THAT(r.prob[2], WithinAbs(0.1625, 1e-12));
}

TEST_CASE("all books unavailable yields no fair price, not a fabricated one", "[consensus]") {
    std::vector<BookInput> books = {
        make({0.60, 0.25, 0.15}, 3.0, false), // stale
        make({0.50, 0.30, 0.20}, 1.0, false), // stale
    };
    auto r = fair_price(books);
    REQUIRE_FALSE(r.has_fair_price);
    REQUIRE(r.books_used == 0);
    REQUIRE(r.total_weight == 0.0);
    // prob must be the zeroed default, not leftover/garbage state.
    REQUIRE(r.prob[0] == 0.0);
    REQUIRE(r.prob[1] == 0.0);
    REQUIRE(r.prob[2] == 0.0);
}

TEST_CASE("single available book returns exactly that book's probabilities", "[consensus]") {
    std::vector<BookInput> books = {
        make({0.70, 0.20, 0.10}, 5.0, true),
        make({0.10, 0.10, 0.80}, 2.0, false), // stale, must not leak in
    };
    auto r = fair_price(books);
    REQUIRE(r.has_fair_price);
    REQUIRE(r.books_used == 1);
    REQUIRE_THAT(r.prob[0], WithinAbs(0.70, 1e-12));
    REQUIRE_THAT(r.prob[1], WithinAbs(0.20, 1e-12));
    REQUIRE_THAT(r.prob[2], WithinAbs(0.10, 1e-12));
}

TEST_CASE("an unavailable book's weight does not leak into total_weight", "[consensus]") {
    std::vector<BookInput> books = {
        make({0.70, 0.20, 0.10}, 5.0, true),
        make({0.10, 0.10, 0.80}, 999.0, false), // huge weight, but stale
    };
    auto r = fair_price(books);
    REQUIRE_THAT(r.total_weight, WithinAbs(5.0, 1e-12));
}

TEST_CASE("zero or negative weight is treated as unavailable", "[consensus]") {
    std::vector<BookInput> books = {
        make({0.70, 0.20, 0.10}, 0.0, true),
        make({0.10, 0.10, 0.80}, -1.0, true),
    };
    auto r = fair_price(books);
    REQUIRE_FALSE(r.has_fair_price);
}

TEST_CASE("a book with a mismatched outcome count is skipped, not averaged in", "[consensus]") {
    BookInput a = make({0.70, 0.20, 0.10}, 1.0, true); // count 3
    BookInput b{};
    b.count = 2; // mismatched
    b.weight = 1.0;
    b.available = true;
    b.prob[0] = 0.5; b.prob[1] = 0.5;
    std::vector<BookInput> books = {a, b};
    auto r = fair_price(books);
    REQUIRE(r.has_fair_price);
    REQUIRE(r.books_used == 1);
    REQUIRE_THAT(r.prob[0], WithinAbs(0.70, 1e-12));
}
