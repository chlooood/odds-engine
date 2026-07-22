#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include "../engine/SessionReader.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: verify_session <session.bin>\n");
        return 2;
    }
    try {
        SessionReader reader(argv[1]);
        const auto& h = reader.header();
        std::printf("header: seed=%llu markets=%u books=%u ticks=%llu records=%llu\n",
                    (unsigned long long)h.seed, h.markets, h.books,
                    (unsigned long long)h.ticks, (unsigned long long)h.record_count);

        std::map<uint32_t, uint64_t> last_seq;
        uint64_t last_ts = 0;
        uint64_t count = 0;
        int failures = 0;

        auto fail = [&](const char* what) {
            if (failures < 10)
                std::fprintf(stderr, "FAIL [%s] at record %llu\n", what, (unsigned long long)count);
            ++failures;
        };

        OddsUpdate u{};
        while (reader.next(u)) {
            if (u.market_id >= h.markets) fail("market_id out of range");
            if (u.outcome_count < 2 || u.outcome_count > 4) fail("bad outcome_count");
            uint32_t slot = ((uint32_t)u.market_id << 16) | u.book_id;
            auto it = last_seq.find(slot);
            if (it != last_seq.end() && u.seq_no <= it->second) fail("seq_no not increasing");
            last_seq[slot] = u.seq_no;
            if (u.sim_timestamp_ns < last_ts) fail("timestamp went backwards");
            last_ts = u.sim_timestamp_ns;
            for (int i = 0; i < 4; ++i) {
                if (i < u.outcome_count) {
                    if (u.odds_milli[i] < 1001 || u.odds_milli[i] > 1000000) fail("odds out of bounds");
                } else {
                    if (u.odds_milli[i] != 0) fail("unused odds slot not zero");
                }
            }
            ++count;
        }
        if (count != h.record_count) fail("record count mismatch");
        if (failures == 0) {
            std::printf("OK: %llu records, all invariants hold\n", (unsigned long long)count);
            return 0;
        }
        std::fprintf(stderr, "%d invariant failures\n", failures);
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
