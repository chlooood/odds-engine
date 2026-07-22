#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

// The latent probabilities, recorded separately from the odds stream.
// The engine never reads this — only the offline validation step does, to
// measure how close each devig method gets to the probability that actually
// generated the prices.
struct alignas(64) TruthRecord {
    uint64_t tick;
    uint16_t market_id;
    uint8_t  outcome_count;
    uint8_t  _pad0[5];
    double   prob[4];
    uint8_t  _pad1[16];
};
static_assert(sizeof(TruthRecord) == 64);

class TruthSink {
public:
    explicit TruthSink(const std::string& path) {
        file_ = std::fopen(path.c_str(), "wb");
        if (!file_) throw std::runtime_error("cannot open truth file: " + path);
    }
    ~TruthSink() { if (file_) std::fclose(file_); }

    TruthSink(const TruthSink&) = delete;
    TruthSink& operator=(const TruthSink&) = delete;

    void accept(const TruthRecord& r) {
        buffer_[fill_++] = r;
        if (fill_ == kBufferRecords) flush();
    }

    void finish() {
        flush();
        std::fclose(file_);
        file_ = nullptr;
    }

private:
    static constexpr size_t kBufferRecords = 4096;

    void flush() {
        if (fill_ == 0) return;
        if (std::fwrite(buffer_.data(), sizeof(TruthRecord), fill_, file_) != fill_)
            throw std::runtime_error("short write to truth file");
        fill_ = 0;
    }

    std::FILE* file_ = nullptr;
    std::array<TruthRecord, kBufferRecords> buffer_{};
    size_t fill_ = 0;
};
