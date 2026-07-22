#pragma once
#include <cstdio>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include "../sim/TruthSink.hpp"

// TruthSink writes a headerless stream of fixed-size TruthRecords (see
// sim/TruthSink.hpp). That asymmetry with SessionHeader is deliberate scope
// control, not an oversight: truth is a validation-only side channel the
// engine never reads, so it never earned a magic/version header of its own.
// Documented in docs/output-sink-abstraction.md.
//
// Loaded eagerly and fully into an index keyed by (tick, market_id), since a
// full session's truth is a few MB at most (see validation-experiment.md for
// the actual sizing) and random lookup by tick is the tool's access pattern.
class TruthReader {
public:
    explicit TruthReader(const std::string& path) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) throw std::runtime_error("cannot open truth file: " + path);

        TruthRecord r{};
        while (std::fread(&r, sizeof(TruthRecord), 1, f) == 1) {
            const uint64_t key = make_key(r.tick, r.market_id);
            index_.emplace(key, r);
            ++count_;
        }
        std::fclose(f);

        if (count_ == 0) throw std::runtime_error("truth file is empty: " + path);
    }

    // Returns nullptr if no truth record exists for this (tick, market_id).
    const TruthRecord* find(uint64_t tick, uint16_t market_id) const {
        auto it = index_.find(make_key(tick, market_id));
        return (it != index_.end()) ? &it->second : nullptr;
    }

    size_t record_count() const { return count_; }

private:
    static uint64_t make_key(uint64_t tick, uint16_t market_id) {
        // tick is bounded well under 2^48 for any realistic session; market_id
        // is 16 bits on the wire (see proto/messages.hpp). Packing avoids a
        // pair-hash and keeps the index a flat unordered_map<uint64_t, ...>.
        return (tick << 16) | static_cast<uint64_t>(market_id);
    }

    std::unordered_map<uint64_t, TruthRecord> index_;
    size_t count_ = 0;
};
