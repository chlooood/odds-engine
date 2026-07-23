#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

// One record per staked outcome. The engine emits these; analytics/analyze_bets
// scores them for closing-line value and (with a settlement file) PnL. Kept
// deliberately flat and fixed-size, same discipline as OddsUpdate: an offline
// tool can mmap the array and never parse.
struct BetRecord {
    uint64_t sim_timestamp_ns;  // when the bet was placed
    uint16_t market_id;
    uint8_t  outcome;           // which outcome was backed
    uint8_t  _pad0[5];
    double   stake;             // bankroll fraction, post fractional-Kelly + cap
    uint32_t odds_milli_taken;  // the price actually bet into (best across books)
    uint32_t _pad1;
    double   fair_prob;         // consensus fair prob for this outcome at bet time
};
static_assert(sizeof(BetRecord) == 40);

struct BetLogHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t record_size;
    uint64_t seed;              // session seed this bet log was produced from
    uint64_t record_count;      // patched in finish()
};
static_assert(sizeof(BetLogHeader) == 24);

inline constexpr uint32_t kBetLogMagic   = 0x5354454BU; // 'KETS' -> bet log
inline constexpr uint16_t kBetLogVersion = 1;

class BetLogSink {
public:
    BetLogSink(const std::string& path, uint64_t seed) {
        file_ = std::fopen(path.c_str(), "wb");
        if (!file_) throw std::runtime_error("cannot open bet log: " + path);
        BetLogHeader h{};
        h.magic = kBetLogMagic; h.version = kBetLogVersion;
        h.record_size = static_cast<uint16_t>(sizeof(BetRecord));
        h.seed = seed; h.record_count = 0;
        if (std::fwrite(&h, sizeof(h), 1, file_) != 1)
            throw std::runtime_error("failed writing bet log header");
    }
    ~BetLogSink() { if (file_) std::fclose(file_); }
    BetLogSink(const BetLogSink&) = delete;
    BetLogSink& operator=(const BetLogSink&) = delete;
    void accept(const BetRecord& r) {
        buffer_[fill_++] = r;
        if (fill_ == kBufferRecords) flush();
        ++record_count_;
    }
    void finish() {
        flush();
        if (std::fseek(file_, static_cast<long>(offsetof(BetLogHeader, record_count)), SEEK_SET) != 0)
            throw std::runtime_error("failed seeking to patch bet count");
        if (std::fwrite(&record_count_, sizeof(record_count_), 1, file_) != 1)
            throw std::runtime_error("failed patching bet count");
        std::fclose(file_); file_ = nullptr;
    }
    uint64_t record_count() const { return record_count_; }
private:
    static constexpr size_t kBufferRecords = 4096;
    void flush() {
        if (fill_ == 0) return;
        if (std::fwrite(buffer_.data(), sizeof(BetRecord), fill_, file_) != fill_)
            throw std::runtime_error("short write to bet log");
        fill_ = 0;
    }
    std::FILE* file_ = nullptr;
    std::array<BetRecord, kBufferRecords> buffer_{};
    size_t fill_ = 0;
    uint64_t record_count_ = 0;
};
