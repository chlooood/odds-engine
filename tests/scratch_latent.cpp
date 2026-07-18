#include <cstdio>
#include "../sim/LatentMarket.hpp"

int main() {
    LatentMarket market(/*seed=*/42, {0.45, 0.27, 0.28});

    for (int tick = 0; tick < 1000; ++tick) {
        auto p = market.step();
        if (tick % 50 == 0) {
            std::printf("tick %4d: home=%.4f draw=%.4f away=%.4f sum=%.6f\n",
                        tick, p[0], p[1], p[2], p[0] + p[1] + p[2]);
        }
    }
    return 0;
}
