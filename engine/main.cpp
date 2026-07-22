#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../proto/messages.hpp"
#include "BookTable.hpp"
#include "Consensus.hpp"
#include "Devig.hpp"
#include "ReplayHash.hpp"
#include "SessionReader.hpp"

namespace {

// Sized to cover any session this project generates without allocating on the
// heap. market_id/book_id are uint16_t on the wire (65535 ceiling), but a flat
// table at that size would be gigabytes; these bounds are the engine process's
// actual capacity and the loop checks the session header against them.
using Table = BookTable<1024, 16>;

enum class DevigMethod { Multiplicative, Power, Shin };

bool parse_devig(const std::string& s, DevigMethod& out) {
    if (s == "multiplicative") { out = DevigMethod::Multiplicative; return true; }
    if (s == "power")          { out = DevigMethod::Power;          return true; }
    if (s == "shin")           { out = DevigMethod::Shin;           return true; }
    return false;
}

devig::DevigResult run_devig(DevigMethod m, std::span<const double> odds) {
    switch (m) {
        case DevigMethod::Multiplicative: return devig::multiplicative(odds);
        case DevigMethod::Power:          return devig::power(odds);
        case DevigMethod::Shin:           return devig::shin(odds);
    }
    return {};
}

struct Options {
    std::string in = "data/session.bin";
    bool hash = false;              // print one digest instead of summaries
    uint64_t stale_ms = 3000;       // a book silent longer than this is excluded
    uint64_t summary_every = 1000;  // print a summary line every N ticks
    // Shin is the default because docs/validation-experiment.md concludes it
    // is the robust choice: never best on any single vig model, but lowest
    // worst-case error across all of them, which is the relevant property when
    // the real shading model is unknown.
    DevigMethod devig_method = DevigMethod::Shin;
    bool help = false;
};

bool parse_args(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", name); return nullptr; }
            return argv[++i];
        };
        if (a == "--in")               { auto v = next("--in");            if (!v) return false; opt.in = v; }
        else if (a == "--hash")        { opt.hash = true; }
        else if (a == "--stale-ms")    { auto v = next("--stale-ms");      if (!v) return false; opt.stale_ms = std::strtoull(v, nullptr, 10); }
        else if (a == "--summary-every"){ auto v = next("--summary-every"); if (!v) return false; opt.summary_every = std::strtoull(v, nullptr, 10); }
        else if (a == "--devig") {
            auto v = next("--devig"); if (!v) return false;
            if (!parse_devig(v, opt.devig_method)) {
                std::fprintf(stderr, "unknown --devig: %s (expected multiplicative|power|shin)\n", v);
                return false;
            }
        }
        else if (a == "--help") {
            opt.help = true;
            std::printf("usage: odds_engine --in PATH [--hash] [--devig multiplicative|power|shin]\n"
                        "                   [--stale-ms N] [--summary-every N]\n"
                        "\n"
                        "  reads a session file, maintains per-book quote state, and on each tick\n"
                        "  boundary computes an equal-weighted consensus fair price per market from\n"
                        "  the non-stale books' devigged probabilities.\n"
                        "\n"
                        "  --hash          print one FNV-1a digest of the whole fair-price trajectory\n"
                        "                  instead of periodic summaries. Same seed -> same digest.\n"
                        "  --devig METHOD  devig method feeding consensus (default: shin, the robust\n"
                        "                  choice per docs/validation-experiment.md).\n"
                        "  --stale-ms N    a book whose last quote is older than N ms is excluded from\n"
                        "                  consensus for that tick (default 3000).\n"
                        "  --summary-every N  print a summary line every N ticks (default 1000).\n"
                        "\n"
                        "  Consensus weights every available book EQUALLY and does NOT read per-book\n"
                        "  sharpness from any config: doing so would hand the engine the simulator's\n"
                        "  own answer. See docs/open-questions.md Q3.\n");
            return true;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) return 1;
    if (opt.help) return 0;

    const uint64_t stale_ns = opt.stale_ms * 1'000'000ULL;

    try {
        SessionReader reader(opt.in);
        const auto& h = reader.header();

        if (h.markets > Table::max_markets()) {
            std::fprintf(stderr, "session has %u markets, engine table holds %zu\n",
                         h.markets, Table::max_markets());
            return 1;
        }
        if (h.books > Table::max_books()) {
            std::fprintf(stderr, "session has %u books, engine table holds %zu\n",
                         h.books, Table::max_books());
            return 1;
        }

        Table table;
        ReplayHash hash;
        uint64_t consensus_count = 0;
        uint64_t no_price_count = 0;

        // Reused across ticks; sized to the header's book count so the inner
        // loop never allocates. Cleared, not reallocated, each market.
        std::vector<consensus::BookInput> inputs(h.books);

        // Tick-boundary detection: records within one tick share a timestamp.
        // When the timestamp advances, the previous tick is fully applied and
        // its consensus can be computed. have_tick guards the first record.
        bool have_tick = false;
        uint64_t cur_ts = 0;

        auto process_tick = [&](uint64_t ts) {
            const uint64_t tick = ts / kTickIntervalNs;
            for (uint16_t m = 0; m < static_cast<uint16_t>(h.markets); ++m) {
                for (uint16_t b = 0; b < static_cast<uint16_t>(h.books); ++b) {
                    consensus::BookInput& in = inputs[b];
                    in = consensus::BookInput{};
                    if (table.is_stale(m, b, ts, stale_ns)) continue;

                    const auto& q = table.get(m, b);
                    std::array<double, devig::kMaxOutcomes> odds{};
                    const int n = q.outcome_count;
                    for (int i = 0; i < n; ++i) odds[i] = q.odds_milli[i] / 1000.0;

                    const auto r = run_devig(opt.devig_method,
                                             std::span<const double>(odds.data(), n));
                    if (!r.usable_as_distribution()) continue;

                    in.count = r.count;
                    for (int i = 0; i < r.count; ++i) in.prob[i] = r.prob[i];
                    in.weight = 1.0;      // equal weight: see Q3 decision above
                    in.available = true;
                }

                const auto fair = consensus::fair_price(inputs);
                if (!fair.has_fair_price) { ++no_price_count; continue; }
                ++consensus_count;

                if (opt.hash) {
                    hash.mix_u16(m);
                    hash.mix_bytes(&fair.count, sizeof(fair.count));
                    for (int i = 0; i < fair.count; ++i) hash.mix_double(fair.prob[i]);
                } else if (opt.summary_every && tick % opt.summary_every == 0) {
                    std::printf("tick %llu market %u fair:", (unsigned long long)tick, m);
                    for (int i = 0; i < fair.count; ++i) std::printf(" %.4f", fair.prob[i]);
                    std::printf("  (%zu books)\n", fair.books_used);
                }
            }
        };

        OddsUpdate u{};
        while (reader.next(u)) {
            if (have_tick && u.sim_timestamp_ns != cur_ts) {
                process_tick(cur_ts);
            }
            cur_ts = u.sim_timestamp_ns;
            have_tick = true;
            table.apply(u, u.sim_timestamp_ns);
        }
        if (have_tick) process_tick(cur_ts); // final tick

        if (opt.hash) {
            std::printf("%016llx\n", (unsigned long long)hash.digest());
        } else {
            std::printf("done: %llu consensus prices computed, %llu ticks*markets had no price, "
                        "%llu records rejected out-of-range, %llu stale-seq rejected\n",
                        (unsigned long long)consensus_count,
                        (unsigned long long)no_price_count,
                        (unsigned long long)table.rejected_out_of_range(),
                        (unsigned long long)table.rejected_stale_seq());
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
