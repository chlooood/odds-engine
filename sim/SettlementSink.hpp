#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

// Realised outcome per market, drawn from a SEPARATE seeded RNG so the draw
// never touches the LatentMarket stream and the session file stays
// byte-identical (the replay-hash determinism gate depends on this). One
// record per market: a betting market resolves to exactly one outcome.
struct SettlementRecord {
    uint16_t market_id;
    uint8_t  winner;        // index of the winning outcome
    uint8_t  outcome_count;
    uint8_t  _pad[4];
};
static_assert(sizeof(SettlementRecord) == 8);

struct SettlementHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t _pad0;
    uint64_t seed;
    uint32_t markets;
    uint32_t _pad1;
};
static_assert(sizeof(SettlementHeader) == 24);

inline constexpr uint32_t kSettlementMagic   = 0x4C545453U; // 'STTL'
inline constexpr uint16_t kSettlementVersion = 1;

class SettlementSink {
public:
    SettlementSink(const std::string& path, uint64_t seed, uint32_t markets) {
        file_ = std::fopen(path.c_str(), "wb");
        if (!file_) throw std::runtime_error("cannot open settlement file: " + path);
        SettlementHeader h{};
        h.magic = kSettlementMagic; h.version = kSettlementVersion;
        h.seed = seed; h.markets = markets;
        if (std::fwrite(&h, sizeof(h), 1, file_) != 1)
            throw std::runtime_error("failed writing settlement header");
    }
    ~SettlementSink() { if (file_) std::fclose(file_); }
    SettlementSink(const SettlementSink&) = delete;
    SettlementSink& operator=(const SettlementSink&) = delete;
    void accept(const SettlementRecord& r) {
        if (std::fwrite(&r, sizeof(r), 1, file_) != 1)
            throw std::runtime_error("short write to settlement file");
    }
    void finish() { std::fclose(file_); file_ = nullptr; }
private:
    std::FILE* file_ = nullptr;
};
