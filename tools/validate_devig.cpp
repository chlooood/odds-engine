#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "../engine/Devig.hpp"
#include "../engine/SessionReader.hpp"
#include "../engine/TruthReader.hpp"
#include "../proto/messages.hpp"
constexpr uint64_t kTickIntervalNs = 100'000'000ULL; // 100 ms

// Measures how accurately each devig method in engine/Devig.hpp recovers the
// latent probability that generated a session's prices. See
// docs/validation-experiment.md for the full writeup, including why
// --calibration on odds_sim is required for a clean reading rather than
// optional.
//
// Bucketing is by TRUE probability (from truth), not by quoted probability,
// because the question is "how good is the method for outcomes that are
// actually longshots", and quoted probability is itself the thing being
// corrected.
namespace {

struct MethodStats {
    // Explicit constructor rather than aggregate init: MethodStats objects are
    // built with only a name and zero-initialized accumulators, and -Werror
    // flagged the partial aggregate init that resulted from adding fields
    // without also adding them to every initializer list.
    explicit MethodStats(std::string n) : name(std::move(n)) {}

    std::string name;
    double abs_sum = 0.0;
    double sq_sum = 0.0;
    double worst = 0.0;
    uint64_t n = 0;

    void add(double err) {
        const double a = std::abs(err);
        abs_sum += a;
        sq_sum += err * err;
        worst = std::max(worst, a);
        ++n;
    }

    double mae() const { return n ? abs_sum / static_cast<double>(n) : 0.0; }
    double rmse() const { return n ? std::sqrt(sq_sum / static_cast<double>(n)) : 0.0; }
};

// Buckets are keyed by the TRUE probability of the specific outcome being
// scored, not by the market's favourite/longshot split, so a favourite's
// draw leg can land in the longshot bucket and vice versa.
enum class Bucket { Longshot, Mid, Favourite, Count };

Bucket classify(double true_prob) {
    if (true_prob < 0.15) return Bucket::Longshot;
    if (true_prob > 0.50) return Bucket::Favourite;
    return Bucket::Mid;
}

const char* bucket_name(Bucket b) {
    switch (b) {
        case Bucket::Longshot:  return "longshot (<0.15)";
        case Bucket::Mid:       return "mid";
        case Bucket::Favourite: return "favourite (>0.50)";
        default:                return "?";
    }
}

struct Options {
    std::string session_path;
    std::string truth_path;
    uint64_t lag_ticks = 0; // truth is looked up at (tick - lag) when nonzero
    bool json = false;
    bool help = false;
};

bool parse_args(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", name); return nullptr; }
            return argv[++i];
        };
        if (a == "--session")   { auto v = next("--session"); if (!v) return false; opt.session_path = v; }
        else if (a == "--truth"){ auto v = next("--truth");   if (!v) return false; opt.truth_path = v; }
        else if (a == "--lag")  { auto v = next("--lag");     if (!v) return false; opt.lag_ticks = std::strtoull(v, nullptr, 10); }
        else if (a == "--json") { opt.json = true; }
        else if (a == "--help") {
            opt.help = true;
            std::printf("usage: validate_devig --session PATH --truth PATH [--lag N] [--json]\n"
                        "\n"
                        "  --lag N   compare each quote against truth at (tick - N) instead of the\n"
                        "            quote's own tick. Use this to separate devig error from staleness\n"
                        "            when the session was NOT generated with --calibration. With\n"
                        "            --calibration sessions lag is 0 and this flag is unnecessary.\n"
                        "  --json    machine-readable output instead of the summary table.\n");
            return true;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return false;
        }
    }
    if (opt.session_path.empty() || opt.truth_path.empty()) {
        std::fprintf(stderr, "--session and --truth are required\n");
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) return 1;
    if (opt.help) return 0;

    try {
        SessionReader session(opt.session_path);
        TruthReader truth(opt.truth_path);

        const auto& h = session.header();
        if (h.books > 1) {
            std::fprintf(stderr,
                "warning: session has %u books. Devig accuracy is reported across all of\n"
                "them pooled together, which mixes each book's lag and noise into the same\n"
                "numbers. Generate with --calibration for a clean single-book reading.\n",
                h.books);
        }

        std::vector<MethodStats> overall;
        overall.emplace_back("multiplicative");
        overall.emplace_back("power");
        overall.emplace_back("shin");

        // [method][bucket]
        std::vector<std::vector<MethodStats>> bucketed(3);
        for (auto& row : bucketed) {
            row.emplace_back("multiplicative");
            row.emplace_back("power");
            row.emplace_back("shin");
        }

        uint64_t matched = 0, unmatched = 0;

        OddsUpdate u{};
        while (session.next(u)) {
            uint64_t tick = u.sim_timestamp_ns / kTickIntervalNs;
            if (tick < opt.lag_ticks) { ++unmatched; continue; }
            tick -= opt.lag_ticks;

            const TruthRecord* t = truth.find(tick, u.market_id);
            if (!t) { ++unmatched; continue; }
            ++matched;

            std::vector<double> odds(u.outcome_count);
            for (int i = 0; i < u.outcome_count; ++i) odds[i] = u.odds_milli[i] / 1000.0;

            const auto rm = devig::multiplicative(odds);
            const auto rp = devig::power(odds);
            const auto rs = devig::shin(odds);

            const devig::DevigResult* results[3] = {&rm, &rp, &rs};
            for (int method = 0; method < 3; ++method) {
                if (!results[method]->usable_as_distribution()) continue;
                for (int i = 0; i < u.outcome_count && i < t->outcome_count; ++i) {
                    const double err = results[method]->prob[i] - t->prob[i];
                    const Bucket b = classify(t->prob[i]);
                    overall[method].add(err);
                    bucketed[static_cast<int>(b)][method].add(err);
                }
            }
        }

        if (matched == 0) {
            std::fprintf(stderr, "no session record matched any truth record; "
                                 "check --lag and that session/truth came from the same run\n");
            return 1;
        }

        if (opt.json) {
            std::printf("{\n  \"matched\": %llu,\n  \"unmatched\": %llu,\n  \"overall\": {\n",
                        (unsigned long long)matched, (unsigned long long)unmatched);
            for (size_t i = 0; i < overall.size(); ++i) {
                const auto& s = overall[i];
                std::printf("    \"%s\": {\"mae\": %.9f, \"rmse\": %.9f, \"worst\": %.9f, \"n\": %llu}%s\n",
                            s.name.c_str(), s.mae(), s.rmse(), s.worst,
                            (unsigned long long)s.n, i + 1 < overall.size() ? "," : "");
            }
            std::printf("  }\n}\n");
            return 0;
        }

        std::printf("matched %llu records against truth (%llu unmatched, skipped)\n\n",
                    (unsigned long long)matched, (unsigned long long)unmatched);

        std::printf("%-16s %12s %12s %12s %10s\n", "method", "MAE", "RMSE", "worst", "n");
        for (const auto& s : overall) {
            std::printf("%-16s %12.6f %12.6f %12.6f %10llu\n",
                        s.name.c_str(), s.mae(), s.rmse(), s.worst, (unsigned long long)s.n);
        }

        for (int b = 0; b < static_cast<int>(Bucket::Count); ++b) {
            std::printf("\n-- %s --\n", bucket_name(static_cast<Bucket>(b)));
            std::printf("%-16s %12s %12s %12s %10s\n", "method", "MAE", "RMSE", "worst", "n");
            for (const auto& s : bucketed[b]) {
                std::printf("%-16s %12.6f %12.6f %12.6f %10llu\n",
                            s.name.c_str(), s.mae(), s.rmse(), s.worst, (unsigned long long)s.n);
            }
        }

        // Identify the best (lowest MAE) method overall as a convenience line,
        // but the docs are explicit that "best on this data" is not the same
        // claim as "best method" - see validation-experiment.md.
        size_t best = 0;
        for (size_t i = 1; i < overall.size(); ++i)
            if (overall[i].mae() < overall[best].mae()) best = i;
        std::printf("\nlowest overall MAE on this run: %s (%.6f)\n",
                    overall[best].name.c_str(), overall[best].mae());

        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
