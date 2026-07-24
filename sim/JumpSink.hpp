#pragma once
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

// Ground-truth jump timeline: one record per (market, tick) at which the latent
// process applied a jump. Recorded by observation only, so it never perturbs the
// market RNG and the session file stays byte-identical. Consumed offline to tag
// each bet with ticks-since-last-jump (docs Q4).
struct JumpRecord {
    uint64_t tick;
    uint16_t market_id;
    uint8_t  _pad[6];
};
static_assert(sizeof(JumpRecord) == 16);

struct JumpHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t _pad0;
    uint64_t seed;
    uint32_t markets;
    uint32_t _pad1;
};
static_assert(sizeof(JumpHeader) == 24);

inline constexpr uint32_t kJumpMagic   = 0x504D554AU; // 'JUMP'
inline constexpr uint16_t kJumpVersion = 1;

class JumpSink {
public:
    JumpSink(const std::string& path, uint64_t seed, uint32_t markets) {
        file_ = std::fopen(path.c_str(), "wb");
        if (!file_) throw std::runtime_error("cannot open jump file: " + path);
        JumpHeader h{};
        h.magic = kJumpMagic; h.version = kJumpVersion; h.seed = seed; h.markets = markets;
        if (std::fwrite(&h, sizeof(h), 1, file_) != 1)
            throw std::runtime_error("failed writing jump header");
    }
    ~JumpSink() { if (file_) std::fclose(file_); }
    JumpSink(const JumpSink&) = delete;
    JumpSink& operator=(const JumpSink&) = delete;
    void accept(uint16_t market_id, uint64_t tick) {
        JumpRecord r{}; r.tick = tick; r.market_id = market_id;
        if (std::fwrite(&r, sizeof(r), 1, file_) != 1)
            throw std::runtime_error("short write to jump file");
    }
    void finish() { std::fclose(file_); file_ = nullptr; }
private:
    std::FILE* file_ = nullptr;
};
