#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../proto/messages.hpp"
#include "Bookmaker.hpp"
#include "LatentMarket.hpp"
#include "OutputSink.hpp"
#include "Seeding.hpp"
#include "TruthSink.hpp"

namespace {

// update_interval_ticks models feed throttling - recreational books publish
// less often than sharp ones, which compounds with lag to widen the gap.
struct SimBookConfig {
    BookConfig cfg;
    int update_interval_ticks;
};

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

// The calibration roster: a single book at zero lag and zero pricing noise,
// updating every tick. Its only source of quoted error is the vig model
// itself, which is what validate_devig needs to isolate devig accuracy from
// staleness and noise. See docs/validation-experiment.md for why this is a
// separate roster rather than a --lag 0 flag on the default one: the default
// roster's five books exist to demonstrate feed heterogeneity, and forcing
// all five to zero lag/noise would silently change what they're modeling.
const std::vector<SimBookConfig>& calibration_roster(VigModel model) {
    static std::vector<SimBookConfig> roster;
    roster = { { {0, 0, 0.060, 0.0, 1.08, model}, 1 } };
    return roster;
}

// Opening lines vary per market so the session isn't five copies of one match.
std::array<double, LatentMarket::kOutcomes> opening_line(uint64_t master, uint32_t market_id) {
    uint64_t h = derive_seed(master, 99, market_id);
    double home = 0.25 + 0.35 * static_cast<double>(h % 1000) / 1000.0;
    double draw = 0.20 + 0.15 * static_cast<double>((h >> 20) % 1000) / 1000.0;
    double away = 1.0 - home - draw;
    if (away < 0.05) { away = 0.05; double s = home + draw + away; home /= s; draw /= s; away /= s; }
    return {home, draw, away};
}

bool parse_vig_model(const std::string& s, VigModel& out) {
    if (s == "power")        { out = VigModel::Power;        return true; }
    if (s == "additive")     { out = VigModel::Additive;     return true; }
    if (s == "proportional") { out = VigModel::Proportional; return true; }
    return false;
}

struct Options {
    uint64_t seed = 42;
    uint32_t markets = 5;
    uint64_t ticks = 10000;
    std::string out = "data/session.bin";
    std::string truth_out; // empty = don't record truth
    VigModel vig_model = VigModel::Power;
    bool vig_model_set = false; // distinguishes "not given" from "given as power"
    bool calibration = false;
    bool help = false;
};

bool parse_args(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", name); return nullptr; }
            return argv[++i];
        };
        if (a == "--seed")         { auto v = next("--seed");    if (!v) return false; opt.seed = std::strtoull(v, nullptr, 10); }
        else if (a == "--markets") { auto v = next("--markets"); if (!v) return false; opt.markets = static_cast<uint32_t>(std::strtoul(v, nullptr, 10)); }
        else if (a == "--ticks")   { auto v = next("--ticks");   if (!v) return false; opt.ticks = std::strtoull(v, nullptr, 10); }
        else if (a == "--out")     { auto v = next("--out");     if (!v) return false; opt.out = v; }
        else if (a == "--truth")   { auto v = next("--truth");   if (!v) return false; opt.truth_out = v; }
        else if (a == "--vig-model") {
            auto v = next("--vig-model"); if (!v) return false;
            if (!parse_vig_model(v, opt.vig_model)) {
                std::fprintf(stderr, "unknown --vig-model: %s (expected power|additive|proportional)\n", v);
                return false;
            }
            opt.vig_model_set = true;
        }
        else if (a == "--calibration") { opt.calibration = true; }
        else if (a == "--help") {
            opt.help = true;
            std::printf("usage: odds_sim [--seed N] [--markets N] [--ticks N] [--out PATH] [--truth PATH]\n"
                        "                 [--vig-model power|additive|proportional] [--calibration]\n"
                        "\n"
                        "  --vig-model    applies the chosen shading model to every book in the roster\n"
                        "                 (default: each book keeps its per-roster default, which is\n"
                        "                 Power). Ignored on its own for anything but comparing devig\n"
                        "                 accuracy across models; see docs/validation-experiment.md.\n"
                        "  --calibration  replaces the roster with a single book at zero lag and zero\n"
                        "                 pricing noise, updating every tick. Required for a clean devig\n"
                        "                 accuracy measurement - without it, measured error also includes\n"
                        "                 staleness and per-book noise.\n");
            return true; // caller checks opt.help and exits 0, not an error
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return false;
        }
    }
    if (opt.markets == 0 || opt.ticks == 0) {
        std::fprintf(stderr, "markets and ticks must be > 0\n");
        return false;
    }
    if (opt.markets > kMaxMarketsSupported) {
        std::fprintf(stderr, "markets exceeds supported maximum (%u)\n", kMaxMarketsSupported);
        return false;
    }
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
    if (opt.help) return 0;

    // Build the effective roster: calibration mode overrides the roster
    // entirely; --vig-model without --calibration overrides only the vig
    // model field of the default roster, keeping each book's lag/noise/
    // overround/skew as tuned.
    std::vector<SimBookConfig> roster;
    if (opt.calibration) {
        roster = calibration_roster(opt.vig_model_set ? opt.vig_model : VigModel::Power);
    } else {
        roster = default_roster();
        if (opt.vig_model_set) {
            for (auto& b : roster) b.cfg.vig_model = opt.vig_model;
        }
    }

    // All state is sized and allocated before the generation loop begins.
    // Nothing allocates inside it.
    std::vector<MarketSim> sims;
    sims.reserve(opt.markets);
    for (uint32_t m = 0; m < opt.markets; ++m) {
        MarketSim sim{
            LatentMarket(derive_seed(opt.seed, seed_domain::kLatentMarket, m),
                         opening_line(opt.seed, m)),
            {}
        };
        sim.books.reserve(roster.size());
        for (size_t b = 0; b < roster.size(); ++b) {
            // Seed depends on both book and market so two markets don't share a
            // noise stream, which would correlate their pricing errors.
            sim.books.emplace_back(roster[b].cfg,
                                   derive_seed(opt.seed, seed_domain::kBookmaker,
                                               m * 1000ULL + b));
        }
        sims.push_back(std::move(sim));
    }

    SessionHeader header{};
    header.magic       = kSessionMagic;
    header.version     = kSessionVersion;
    header.record_size = static_cast<uint16_t>(sizeof(OddsUpdate));
    header.seed        = opt.seed;
    header.markets     = opt.markets;
    header.books       = static_cast<uint32_t>(roster.size());
    header.ticks       = opt.ticks;

    std::unique_ptr<FileSink> sink;
    std::unique_ptr<TruthSink> truth;
    try {
        sink = std::make_unique<FileSink>(opt.out, header);
        if (!opt.truth_out.empty()) truth = std::make_unique<TruthSink>(opt.truth_out);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }

    // Per-book sequence numbers span all markets, mirroring a real venue that
    // emits one sequence across every instrument it quotes. Gaps within a
    // (market, book) slot are therefore expected and harmless.
    std::vector<uint64_t> seq_no(roster.size(), 0);

    OddsUpdate update{};
    TruthRecord truth_rec{};

    try {
        for (uint64_t tick = 0; tick < opt.ticks; ++tick) {
            const uint64_t ts = tick * kTickIntervalNs;

            for (uint32_t m = 0; m < opt.markets; ++m) {
                auto& sim = sims[m];
                const auto latent = sim.latent.step();

                if (truth) {
                    truth_rec = TruthRecord{};
                    truth_rec.tick = tick;
                    truth_rec.market_id = static_cast<uint16_t>(m);
                    truth_rec.outcome_count = LatentMarket::kOutcomes;
                    for (int i = 0; i < LatentMarket::kOutcomes; ++i)
                        truth_rec.prob[i] = latent[i];
                    truth->accept(truth_rec);
                }

                for (size_t b = 0; b < sim.books.size(); ++b) {
                    // observe() runs every tick regardless of publication rate:
                    // the book's internal view of the world advances even when
                    // it isn't republishing a price.
                    sim.books[b].observe(latent);

                    if (tick % static_cast<uint64_t>(roster[b].update_interval_ticks) != 0) continue;

                    const auto odds = sim.books[b].quote();

                    // Zeroed each time, not reused: unused odds slots must be
                    // zero for two semantically identical runs to produce
                    // identical bytes.
                    update = OddsUpdate{};
                    update.seq_no = seq_no[b]++;
                    update.sim_timestamp_ns = ts;
                    update.book_id = roster[b].cfg.book_id;
                    update.market_id = static_cast<uint16_t>(m);
                    update.outcome_count = LatentMarket::kOutcomes;
                    for (int i = 0; i < LatentMarket::kOutcomes; ++i)
                        update.odds_milli[i] = odds[i];

                    sink->accept(update);
                }
            }
        }

        sink->finish();
        if (truth) truth->finish();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error during generation: %s\n", e.what());
        return 1;
    }

    std::printf("wrote %llu records to %s (seed=%llu markets=%u books=%zu ticks=%llu)\n",
                static_cast<unsigned long long>(sink->record_count()), opt.out.c_str(),
                static_cast<unsigned long long>(opt.seed), opt.markets,
                roster.size(), static_cast<unsigned long long>(opt.ticks));
    return 0;
}
