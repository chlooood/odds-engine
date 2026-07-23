#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../proto/messages.hpp"
#include "BetLog.hpp"
#include "BookTable.hpp"
#include "Consensus.hpp"
#include "Devig.hpp"
#include "Kelly.hpp"
#include "ReplayHash.hpp"
#include "SessionReader.hpp"

namespace {

// Must match the simulator's tick interval; used only to turn a timestamp back
// into a human-readable tick number for summary output.
constexpr uint64_t kTickIntervalNs = 100'000'000ULL; // 100 ms
constexpr uint32_t kMinOddsMilli = 1001;              // a real, bettable quote

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
    std::string bets_out;           // NEW: empty = don't emit a bet log
    bool hash = false;
    uint64_t stale_ms = 3000;
    uint64_t summary_every = 1000;
    DevigMethod devig_method = DevigMethod::Shin;
    double kelly_fraction = 0.25;   // NEW: quarter-Kelly default
    double max_exposure = 0.05;     // NEW: cap total stake per market-tick at 5%
    bool help = false;
};

bool parse_args(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", name); return nullptr; }
            return argv[++i];
        };
        if (a == "--in")                { auto v = next("--in");             if (!v) return false; opt.in = v; }
        else if (a == "--bets")         { auto v = next("--bets");           if (!v) return false; opt.bets_out = v; }
        else if (a == "--hash")         { opt.hash = true; }
        else if (a == "--stale-ms")     { auto v = next("--stale-ms");       if (!v) return false; opt.stale_ms = std::strtoull(v, nullptr, 10); }
        else if (a == "--summary-every"){ auto v = next("--summary-every");  if (!v) return false; opt.summary_every = std::strtoull(v, nullptr, 10); }
        else if (a == "--kelly-fraction"){auto v = next("--kelly-fraction"); if (!v) return false; opt.kelly_fraction = std::strtod(v, nullptr); }
        else if (a == "--max-exposure") { auto v = next("--max-exposure");   if (!v) return false; opt.max_exposure = std::strtod(v, nullptr); }
        else if (a == "--devig") {
            auto v = next("--devig"); if (!v) return false;
            if (!parse_devig(v, opt.devig_method)) {
                std::fprintf(stderr, "unknown --devig: %s (expected multiplicative|power|shin)\n", v);
                return false;
            }
        }
        else if (a == "--help") {
            opt.help = true;
            std::printf("usage: odds_engine --in PATH [--bets PATH] [--hash]\n"
                        "                   [--devig multiplicative|power|shin]\n"
                        "                   [--kelly-fraction F] [--max-exposure F]\n"
                        "                   [--stale-ms N] [--summary-every N]\n"
                        "\n"
                        "  reads a session file, maintains per-book quote state, and on each tick\n"
                        "  boundary computes an equal-weighted consensus fair price per market from\n"
                        "  the non-stale books' devigged probabilities. With --bets it also sizes a\n"
                        "  fractional-Kelly bet against the best available odds and logs each stake.\n"
                        "\n"
                        "  --bets PATH        emit a bet log for analytics/analyze_bets to score.\n"
                        "  --kelly-fraction F fractional-Kelly multiplier (default 0.25).\n"
                        "  --max-exposure F   cap on total staked fraction per market-tick (default 0.05).\n"
                        "  --hash             print one FNV-1a digest of the whole fair-price trajectory.\n"
                        "  --devig METHOD     devig method feeding consensus (default: shin).\n"
                        "  --stale-ms N       exclude a book quoting older than N ms (default 3000).\n"
                        "  --summary-every N  print a summary line every N ticks (default 1000).\n");
            return true;
        } else { std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); return false; }
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
            std::fprintf(stderr, "session has %u markets, engine table holds %zu\n", h.markets, Table::max_markets());
            return 1;
        }
        if (h.books > Table::max_books()) {
            std::fprintf(stderr, "session has %u books, engine table holds %zu\n", h.books, Table::max_books());
            return 1;
        }

        Table table;
        ReplayHash hash;
        uint64_t consensus_count = 0;
        uint64_t no_price_count = 0;
        uint64_t bets_placed = 0;

        std::unique_ptr<BetLogSink> betlog;
        if (!opt.bets_out.empty()) betlog = std::make_unique<BetLogSink>(opt.bets_out, h.seed);

        std::vector<consensus::BookInput> inputs(h.books);

        bool have_tick = false;
        uint64_t cur_ts = 0;

        auto process_tick = [&](uint64_t ts) {
            const uint64_t tick = ts / kTickIntervalNs;
            for (uint16_t m = 0; m < static_cast<uint16_t>(h.markets); ++m) {
                // Best available (highest) odds per outcome across non-stale books:
                // the actual price you could bet into right now. Tracked alongside
                // the devig-for-consensus pass so books are walked once.
                std::array<uint32_t, devig::kMaxOutcomes> best_odds_milli{};

                for (uint16_t b = 0; b < static_cast<uint16_t>(h.books); ++b) {
                    consensus::BookInput& in = inputs[b];
                    in = consensus::BookInput{};
                    if (table.is_stale(m, b, ts, stale_ns)) continue;

                    const auto& q = table.get(m, b);
                    const int n = q.outcome_count;
                    for (int i = 0; i < n && i < devig::kMaxOutcomes; ++i)
                        if (q.odds_milli[i] > best_odds_milli[i]) best_odds_milli[i] = q.odds_milli[i];

                    std::array<double, devig::kMaxOutcomes> odds{};
                    for (int i = 0; i < n; ++i) odds[i] = q.odds_milli[i] / 1000.0;

                    const auto r = run_devig(opt.devig_method, std::span<const double>(odds.data(), n));
                    if (!r.usable_as_distribution()) continue;

                    in.count = r.count;
                    for (int i = 0; i < r.count; ++i) in.prob[i] = r.prob[i];
                    in.weight = 1.0;
                    in.available = true;
                }

                const auto fair = consensus::fair_price(inputs);
                if (!fair.has_fair_price) { ++no_price_count; continue; }
                ++consensus_count;

                // Sizing: only when a bet log is requested. Every outcome must
                // have a real quote to bet into; otherwise skip this market.
                if (betlog) {
                    std::array<double, devig::kMaxOutcomes> best_dec{};
                    bool have_all = true;
                    for (int i = 0; i < fair.count; ++i) {
                        if (best_odds_milli[i] < kMinOddsMilli) { have_all = false; break; }
                        best_dec[i] = best_odds_milli[i] / 1000.0;
                    }
                    if (have_all) {
                        const auto alloc = kelly::allocate(
                            std::span<const double>(fair.prob.data(), fair.count),
                            std::span<const double>(best_dec.data(), fair.count),
                            opt.kelly_fraction, opt.max_exposure);
                        if (alloc.has_bet) {
                            for (int i = 0; i < fair.count; ++i) {
                                if (alloc.stake[i] <= 0.0) continue;
                                BetRecord br{};
                                br.sim_timestamp_ns = ts;
                                br.market_id = m;
                                br.outcome = static_cast<uint8_t>(i);
                                br.stake = alloc.stake[i];
                                br.odds_milli_taken = best_odds_milli[i];
                                br.fair_prob = fair.prob[i];
                                betlog->accept(br);
                                ++bets_placed;
                            }
                        }
                    }
                }

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
            if (have_tick && u.sim_timestamp_ns != cur_ts) process_tick(cur_ts);
            cur_ts = u.sim_timestamp_ns;
            have_tick = true;
            table.apply(u, u.sim_timestamp_ns);
        }
        if (have_tick) process_tick(cur_ts);

        if (betlog) betlog->finish();

        if (opt.hash) {
            std::printf("%016llx\n", (unsigned long long)hash.digest());
        } else {
            std::printf("done: %llu consensus prices computed, %llu ticks*markets had no price, "
                        "%llu records rejected out-of-range, %llu stale-seq rejected",
                        (unsigned long long)consensus_count, (unsigned long long)no_price_count,
                        (unsigned long long)table.rejected_out_of_range(),
                        (unsigned long long)table.rejected_stale_seq());
            if (betlog) std::printf(", %llu bets logged", (unsigned long long)bets_placed);
            std::printf("\n");
        }
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "error: %s\n", e.what()); return 1; }
}
