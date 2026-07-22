#pragma once
#include <cstdio>
#include <stdexcept>
#include <string>
#include "../proto/messages.hpp"
#include "../sim/OutputSink.hpp"

class SessionReader {
public:
    explicit SessionReader(const std::string& path) {
        file_ = std::fopen(path.c_str(), "rb");
        if (!file_) throw std::runtime_error("cannot open session file: " + path);
        if (std::fread(&header_, sizeof(header_), 1, file_) != 1)
            throw std::runtime_error("session file too short for header");
        if (header_.magic != kSessionMagic)
            throw std::runtime_error("not a session file (bad magic)");
        if (header_.version != kSessionVersion)
            throw std::runtime_error("unsupported session version");
        if (header_.record_size != sizeof(OddsUpdate))
            throw std::runtime_error("record size mismatch");
        if (header_.record_count == 0)
            throw std::runtime_error("record count is zero: session was truncated");
    }
    ~SessionReader() { if (file_) std::fclose(file_); }
    SessionReader(const SessionReader&) = delete;
    SessionReader& operator=(const SessionReader&) = delete;

    const SessionHeader& header() const { return header_; }

    bool next(OddsUpdate& out) {
        if (read_ >= header_.record_count) return false;
        if (std::fread(&out, sizeof(OddsUpdate), 1, file_) != 1)
            throw std::runtime_error("session file truncated before declared record count");
        ++read_;
        return true;
    }

private:
    std::FILE* file_ = nullptr;
    SessionHeader header_{};
    uint64_t read_ = 0;
};
