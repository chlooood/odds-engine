#pragma once
#include <utility>
#include <vector>
#include "OutputSink.hpp"

// Tee. Forwards each record to several sinks; non-owning, the caller keeps them
// alive.
//
// This exists so --udp does not REPLACE the file output. The session file is
// still written byte-for-byte identically while the same records go out on the
// wire, which is what keeps the replay-determinism gate meaningful for runs
// that also broadcast. Sink order is the caller's: put FileSink first so the
// on-disk artefact completes even if the socket errors.
class FanoutSink final : public OutputSink {
public:
    explicit FanoutSink(std::vector<OutputSink*> sinks) : sinks_(std::move(sinks)) {}

    void accept(const OddsUpdate& update) override {
        for (auto* s : sinks_) s->accept(update);
    }
    void finish() override {
        for (auto* s : sinks_) s->finish();
    }

private:
    std::vector<OutputSink*> sinks_;
};
