#include <cstdio>
#include <vector>
#include "../sim/Bookmaker.hpp"
#include "../sim/LatentMarket.hpp"

int main() {
    LatentMarket market(/*seed=*/42, {0.45, 0.27, 0.28});

    std::vector<BookConfig> configs = {
        {0,   1, 0.020, 0.010, 1.00}, // sharp: near-zero lag, 2% vig, no skew
        {1,   5, 0.030, 0.020, 1.02},
        {2,  20, 0.045, 0.035, 1.05},
        {3,  60, 0.060, 0.050, 1.08},
        {4, 150, 0.080, 0.070, 1.12}, // soft: very laggy, 8% vig, heavy skew
    };

    std::vector<Bookmaker> books;
    for (size_t i = 0; i < configs.size(); ++i) {
        books.emplace_back(configs[i], /*seed=*/1000 + i);
    }

    for (int tick = 0; tick < 400; ++tick) {
        auto latent = market.step();
        for (auto& b : books) b.observe(latent);

        if (tick % 100 == 0 || tick == 399) {
            std::printf("\n=== tick %d  latent: %.4f %.4f %.4f ===\n",
                        tick, latent[0], latent[1], latent[2]);
            for (auto& b : books) {
                auto o = b.quote();
                double overround = 1000.0/o[0] + 1000.0/o[1] + 1000.0/o[2];
                std::printf("  book %u: %.3f %.3f %.3f   (implied sum = %.4f)\n",
                            b.book_id(), o[0]/1000.0, o[1]/1000.0, o[2]/1000.0, overround);
            }
        }
    }
    return 0;
}
