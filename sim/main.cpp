#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "../proto/messages.hpp"
#include "Bookmaker.hpp"
#include "LatentMarket.hpp"
#include "OutputSink.hpp"
#include "JumpSink.hpp"
#include "Seeding.hpp"
#include "SettlementSink.hpp"
#include "TruthSink.hpp"

namespace {

constexpr uint64_t kTickIntervalNs = 100'000'000ULL; // 100 ms

struct SimBookConfig { BookConfig cfg; int update_interval_ticks; };

const std::vector<SimBookConfig>& default_roster() {
    static const std::vector<SimBookConfig> roster = {
        {{0,   1, 0.020, 0.010, 1.00},  1},
        {{1,   5, 0.030, 0.020, 1.02},  2},
        {{2,  20, 0.045, 0.035, 1.05},  5},
        {{3,  60, 0.060, 0.050, 1.08}, 10},
        {{4, 150, 0.080, 0.070, 1.12}, 25},
    };
    return roster;
}

std::array<double, LatentMarket::kOutcomes> opening_line(uint64_t master, uint32_t market_id) {
    uint64_t h = derive_seed(master, 99, market_id);
    double home = 0.25 + 0.35 * static_cast<double>(h % 1000) / 1000.0;
    double draw = 0.20 + 0.15 * static_cast<double>((h >> 20) % 1000) / 1000.0;
    double away = 1.0 - home - draw;
    if (away < 0.05) { away = 0.05; double s = home + draw + away; home /= s; draw /= s; away /= s; }
    return {home, draw, away};
}

struct Options {
    uint64_t seed = 42;
    uint32_t markets = 5;
    uint64_t ticks = 10000;
    std::string out = "data/session.bin";
    std::string truth_out;
    std::string settle_out;   // NEW: empty = don't record settlement
    std::string jumps_out;    // NEW: empty = don't record jump timeline
};

bool parse_args(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", name); return nullptr; }
            return argv[++i];
        };
        if (a == "--seed")          { auto v = next("--seed");    if (!v) return false; opt.seed = std::strtoull(v, nullptr, 10); }
        else if (a == "--markets")  { auto v = next("--markets"); if (!v) return false; opt.markets = static_cast<uint32_t>(std::strtoul(v, nullptr, 10)); }
        else if (a == "--ticks")    { auto v = next("--ticks");   if (!v) return false; opt.ticks = std::strtoull(v, nullptr, 10); }
        else if (a == "--out")      { auto v = next("--out");     if (!v) return false; opt.out = v; }
        else if (a == "--truth")    { auto v = next("--truth");   if (!v) return false; opt.truth_out = v; }
        else if (a == "--settle")   { auto v = next("--settle");  if (!v) return false; opt.settle_out = v; }
        else if (a == "--jumps")    { auto v = next("--jumps");   if (!v) return false; opt.jumps_out = v; }
        else if (a == "--help") {
            std::printf("usage: odds_sim [--seed N] [--markets N] [--ticks N] [--out PATH] [--truth PATH] [--settle PATH] [--jumps PATH]\n");
            return false;
        } else { std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); return false; }
    }
    if (opt.markets == 0 || opt.ticks == 0) { std::fprintf(stderr, "markets and ticks must be > 0\n"); return false; }
    if (opt.markets > kMaxMarketsSupported) { std::fprintf(stderr, "markets exceeds supported maximum (%u)\n", kMaxMarketsSupported); return false; }
    return true;
}

struct MarketSim {
    LatentMarket latent;
    std::vector<Bookmaker> books;
};

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) return 1;

    const auto& roster = default_roster();

    std::vector<MarketSim> sims;
    sims.reserve(opt.markets);
    for (uint32_t m = 0; m < opt.markets; ++m) {
        MarketSim sim{
            LatentMarket(derive_seed(opt.seed, seed_domain::kLatentMarket, m), opening_line(opt.seed, m)),
            {}
        };
        sim.books.reserve(roster.size());
        for (size_t b = 0; b < roster.size(); ++b)
            sim.books.emplace_back(roster[b].cfg, derive_seed(opt.seed, seed_domain::kBookmaker, m * 1000ULL + b));
        sims.push_back(std::move(sim));
    }

    SessionHeader header{};
    header.magic = kSessionMagic; header.version = kSessionVersion;
    header.record_size = static_cast<uint16_t>(sizeof(OddsUpdate));
    header.seed = opt.seed; header.markets = opt.markets;
    header.books = static_cast<uint32_t>(roster.size()); header.ticks = opt.ticks;

    std::unique_ptr<FileSink> sink;
    std::unique_ptr<TruthSink> truth;
    std::unique_ptr<JumpSink> jumps;
    try {
        sink = std::make_unique<FileSink>(opt.out, header);
        if (!opt.truth_out.empty()) truth = std::make_unique<TruthSink>(opt.truth_out);
        if (!opt.jumps_out.empty()) jumps = std::make_unique<JumpSink>(opt.jumps_out, opt.seed, opt.markets);
    } catch (const std::exception& e) { std::fprintf(stderr, "error: %s\n", e.what()); return 1; }

    std::vector<uint64_t> seq_no(roster.size(), 0);
    OddsUpdate update{};
    TruthRecord truth_rec{};

    // NEW: capture the final latent distribution per market for settlement.
    // Written only when --settle is set. Drawn from a separate RNG, never the
    // market stream, so the session bytes are unchanged.
    std::vector<std::array<double, LatentMarket::kOutcomes>> final_latent(opt.markets);

    try {
        for (uint64_t tick = 0; tick < opt.ticks; ++tick) {
            const uint64_t ts = tick * kTickIntervalNs;
            for (uint32_t m = 0; m < opt.markets; ++m) {
                auto& sim = sims[m];
                const auto latent = sim.latent.step();
                final_latent[m] = latent;
                if (jumps && sim.latent.jumped()) jumps->accept(static_cast<uint16_t>(m), tick);

                if (truth) {
                    truth_rec = TruthRecord{};
                    truth_rec.tick = tick; truth_rec.market_id = static_cast<uint16_t>(m);
                    truth_rec.outcome_count = LatentMarket::kOutcomes;
                    for (int i = 0; i < LatentMarket::kOutcomes; ++i) truth_rec.prob[i] = latent[i];
                    truth->accept(truth_rec);
                }

                for (size_t b = 0; b < sim.books.size(); ++b) {
                    sim.books[b].observe(latent);
                    if (tick % static_cast<uint64_t>(roster[b].update_interval_ticks) != 0) continue;
                    const auto odds = sim.books[b].quote();
                    update = OddsUpdate{};
                    update.seq_no = seq_no[b]++;
                    update.sim_timestamp_ns = ts;
                    update.book_id = roster[b].cfg.book_id;
                    update.market_id = static_cast<uint16_t>(m);
                    update.outcome_count = LatentMarket::kOutcomes;
                    for (int i = 0; i < LatentMarket::kOutcomes; ++i) update.odds_milli[i] = odds[i];
                    sink->accept(update);
                }
            }
        }

        sink->finish();
        if (truth) truth->finish();
        if (jumps) jumps->finish();

        if (!opt.settle_out.empty()) {
            SettlementSink settle(opt.settle_out, opt.seed, opt.markets);
            for (uint32_t m = 0; m < opt.markets; ++m) {
                // Separate seeded RNG per market: the winner is a draw from the
                // TRUE terminal distribution, using its own stream so it can
                // never perturb the odds the books already emitted.
                std::mt19937_64 srng(derive_seed(opt.seed, seed_domain::kSettlement, m));
                std::discrete_distribution<int> pick(final_latent[m].begin(), final_latent[m].end());
                SettlementRecord r{};
                r.market_id = static_cast<uint16_t>(m);
                r.winner = static_cast<uint8_t>(pick(srng));
                r.outcome_count = LatentMarket::kOutcomes;
                settle.accept(r);
            }
            settle.finish();
        }
    } catch (const std::exception& e) { std::fprintf(stderr, "error during generation: %s\n", e.what()); return 1; }

    std::printf("wrote %llu records to %s (seed=%llu markets=%u books=%zu ticks=%llu)%s\n",
                static_cast<unsigned long long>(sink->record_count()), opt.out.c_str(),
                static_cast<unsigned long long>(opt.seed), opt.markets,
                roster.size(), static_cast<unsigned long long>(opt.ticks),
                opt.settle_out.empty() ? "" : " +settlement");
    return 0;
}
