#include <cstdio>
#include <cstring>
#include <cassert>
#include "../proto/messages.hpp"

int main() {
    OddsUpdate out{};
    out.seq_no = 42;
    out.sim_timestamp_ns = 1234567890;
    out.book_id = 3;
    out.market_id = 7;
    out.outcome_count = 3;
    out.odds_milli[0] = 2150; // 2.150
    out.odds_milli[1] = 3400; // 3.400
    out.odds_milli[2] = 4100; // 4.100
    out.odds_milli[3] = 0;    // unused

    FILE* f = std::fopen("test.bin", "wb");
    assert(f && "failed to open test.bin for write");
    std::fwrite(&out, sizeof(OddsUpdate), 1, f);
    std::fclose(f);

    OddsUpdate in{};
    f = std::fopen("test.bin", "rb");
    assert(f && "failed to open test.bin for read");
    size_t n = std::fread(&in, sizeof(OddsUpdate), 1, f);
    std::fclose(f);
    assert(n == 1 && "did not read exactly one record");

    assert(in.seq_no == out.seq_no);
    assert(in.sim_timestamp_ns == out.sim_timestamp_ns);
    assert(in.book_id == out.book_id);
    assert(in.market_id == out.market_id);
    assert(in.outcome_count == out.outcome_count);
    assert(std::memcmp(in.odds_milli, out.odds_milli, sizeof(out.odds_milli)) == 0);

    std::printf("round-trip OK, sizeof(OddsUpdate) = %zu bytes\n", sizeof(OddsUpdate));
    return 0;
}
