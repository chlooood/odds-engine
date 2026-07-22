#pragma once
#include <cstdint>
#include <cstdio>
#include <cstddef>
#include <array>
#include <stdexcept>
#include <string>
#include "../proto/messages.hpp"

struct alignas(64) SessionHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t record_size;
    uint64_t seed;
    uint32_t markets;
    uint32_t books;
    uint64_t ticks;
    uint64_t record_count;
    uint8_t  _pad[24];
};
static_assert(sizeof(SessionHeader) == 64);

inline constexpr uint32_t kSessionMagic   = 0x5344444FU; // 'ODDS' little-endian
inline constexpr uint16_t kSessionVersion = 1;

// Transport boundary. The generation loop produces records and knows nothing
// about where they go; Stage 2 adds a UDP implementation without touching it.
// Virtual dispatch is acceptable here because the simulator is a data generator,
// not a latency-critical path. The engine's read path uses no virtual calls.
class OutputSink {
public:
    virtual ~OutputSink() = default;
    virtual void accept(const OddsUpdate& update) = 0;
    virtual void finish() = 0;
};

class FileSink final : public OutputSink {
public:
    FileSink(const std::string& path, const SessionHeader& header) {
        file_ = std::fopen(path.c_str(), "wb");
        if (!file_) throw std::runtime_error("cannot open output file: " + path);

        SessionHeader h = header;
        h.record_count = 0; // patched in finish(); a crashed run leaves it zero,
                            // which readers treat as truncated rather than valid
        if (std::fwrite(&h, sizeof(h), 1, file_) != 1)
            throw std::runtime_error("failed writing session header");
    }

    ~FileSink() override {
        if (file_) std::fclose(file_);
    }

    FileSink(const FileSink&) = delete;
    FileSink& operator=(const FileSink&) = delete;

    void accept(const OddsUpdate& update) override {
        buffer_[fill_++] = update;
        if (fill_ == kBufferRecords) flush();
        ++record_count_;
    }

    void finish() override {
        flush();
        if (std::fseek(file_, static_cast<long>(offsetof(SessionHeader, record_count)), SEEK_SET) != 0)
            throw std::runtime_error("failed seeking to patch record count");
        if (std::fwrite(&record_count_, sizeof(record_count_), 1, file_) != 1)
            throw std::runtime_error("failed patching record count");
        std::fclose(file_);
        file_ = nullptr;
    }

    uint64_t record_count() const { return record_count_; }

private:
    // One fwrite per 4096 records rather than per record: same bytes on disk,
    // ~3 orders of magnitude fewer syscalls.
    static constexpr size_t kBufferRecords = 4096;

    void flush() {
        if (fill_ == 0) return;
        if (std::fwrite(buffer_.data(), sizeof(OddsUpdate), fill_, file_) != fill_)
            throw std::runtime_error("short write to session file");
        fill_ = 0;
    }

    std::FILE* file_ = nullptr;
    std::array<OddsUpdate, kBufferRecords> buffer_{};
    size_t fill_ = 0;
    uint64_t record_count_ = 0;
};
