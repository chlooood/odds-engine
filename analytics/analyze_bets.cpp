#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>

#include "../proto/messages.hpp"
#include "../engine/SessionReader.hpp"
#include "../engine/BetLog.hpp"
#include "../sim/SettlementSink.hpp"
#include "../sim/JumpSink.hpp"

// Scores a bet log against the session it was produced from.
//
//   CLV        per-bet, no settlement needed: odds taken vs closing best odds.
//   PnL        needs --settle: realise each market against its drawn winner.
//   post-jump  needs --jumps: tag each bet with ticks-since-last-jump and show
//              WHERE in time the edge lives, plus an effective-sample count at
//              jump-episode granularity. This is the docs Q4 question: if edge
//              concentrates right after jumps, the independent sample driving
//              the edge estimate is the jump count, not the (vast) bet count.

namespace {
constexpr uint64_t kTickIntervalNs = 100'000'000ULL; // must match the simulator

struct Args {
    std::string bets, session, settle, jumps;
};
bool parse(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto next = [&](const char* n) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", n); return nullptr; }
            return argv[++i];
        };
        if (s == "--bets")         { auto v = next("--bets");    if (!v) return false; a.bets = v; }
        else if (s == "--session") { auto v = next("--session"); if (!v) return false; a.session = v; }
        else if (s == "--settle")  { auto v = next("--settle");  if (!v) return false; a.settle = v; }
        else if (s == "--jumps")   { auto v = next("--jumps");   if (!v) return false; a.jumps = v; }
        else { std::fprintf(stderr, "unknown arg: %s\n", s.c_str()); return false; }
    }
    if (a.bets.empty() || a.session.empty()) {
        std::fprintf(stderr, "usage: analyze_bets --bets PATH --session PATH [--settle PATH] [--jumps PATH]\n");
        return false;
    }
    return true;
}

// ticks-since-last-jump: -1 means no jump has occurred in this market yet.
int64_t ticks_since_jump(const std::vector<uint64_t>& jt, uint64_t tick) {
    if (jt.empty()) return -1;
    // largest jump tick <= tick
    auto it = std::upper_bound(jt.begin(), jt.end(), tick);
    if (it == jt.begin()) return -1;
    return static_cast<int64_t>(tick) - static_cast<int64_t>(*(it - 1));
}

const char* kBucketLabel[8] = {
    "0-9", "10-24", "25-49", "50-99", "100-199", "200-499", "500+", "no-jump-yet"
};
int bucket_of(int64_t tsj) {
    if (tsj < 0)    return 7;
    if (tsj < 10)   return 0;
    if (tsj < 25)   return 1;
    if (tsj < 50)   return 2;
    if (tsj < 100)  return 3;
    if (tsj < 200)  return 4;
    if (tsj < 500)  return 5;
    return 6;
}
} // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse(argc, argv, args)) return 2;

    try {
        SessionReader reader(args.session);
        const auto& h = reader.header();
        const size_t M = h.markets, B = h.books;

        std::vector<std::array<uint32_t,4>> last_odds(M * B);
        std::vector<uint8_t> last_count(M * B, 0);
        OddsUpdate u{};
        while (reader.next(u)) {
            if (u.market_id >= M || u.book_id >= B) continue;
            const size_t idx = static_cast<size_t>(u.market_id) * B + u.book_id;
            for (int i = 0; i < 4; ++i) last_odds[idx][i] = u.odds_milli[i];
            last_count[idx] = u.outcome_count;
        }
        std::vector<std::array<uint32_t,4>> closing_best(M);
        for (size_t m = 0; m < M; ++m) {
            closing_best[m] = {0,0,0,0};
            for (size_t b = 0; b < B; ++b) {
                const size_t idx = m * B + b;
                for (int i = 0; i < last_count[idx] && i < 4; ++i)
                    if (last_odds[idx][i] > closing_best[m][i]) closing_best[m][i] = last_odds[idx][i];
            }
        }

        std::vector<int> winner;
        bool have_settle = false;
        if (!args.settle.empty()) {
            std::FILE* f = std::fopen(args.settle.c_str(), "rb");
            if (!f) throw std::runtime_error("cannot open settlement file");
            SettlementHeader sh{};
            if (std::fread(&sh, sizeof(sh), 1, f) != 1) throw std::runtime_error("settlement header short");
            if (sh.magic != kSettlementMagic) throw std::runtime_error("bad settlement magic");
            winner.assign(M, -1);
            SettlementRecord sr{};
            while (std::fread(&sr, sizeof(sr), 1, f) == 1)
                if (sr.market_id < M) winner[sr.market_id] = sr.winner;
            std::fclose(f);
            have_settle = true;
        }

        std::vector<std::vector<uint64_t>> jump_ticks;
        bool have_jumps = false;
        if (!args.jumps.empty()) {
            std::FILE* f = std::fopen(args.jumps.c_str(), "rb");
            if (!f) throw std::runtime_error("cannot open jump file");
            JumpHeader jh{};
            if (std::fread(&jh, sizeof(jh), 1, f) != 1) throw std::runtime_error("jump header short");
            if (jh.magic != kJumpMagic) throw std::runtime_error("bad jump magic");
            jump_ticks.assign(M, {});
            JumpRecord jr{};
            while (std::fread(&jr, sizeof(jr), 1, f) == 1)
                if (jr.market_id < M) jump_ticks[jr.market_id].push_back(jr.tick);
            std::fclose(f);
            for (auto& v : jump_ticks) std::sort(v.begin(), v.end());
            have_jumps = true;
        }

        std::FILE* bf = std::fopen(args.bets.c_str(), "rb");
        if (!bf) throw std::runtime_error("cannot open bet log");
        BetLogHeader bh{};
        if (std::fread(&bh, sizeof(bh), 1, bf) != 1) throw std::runtime_error("bet header short");
        if (bh.magic != kBetLogMagic) throw std::runtime_error("bad bet log magic");
        if (bh.record_size != sizeof(BetRecord)) throw std::runtime_error("bet record size mismatch");

        uint64_t n_bets = 0, n_clv = 0, beat = 0;
        double total_stake = 0.0, clv_sum = 0.0, clv_stake_sum = 0.0, stake_for_clv = 0.0, beat_stake = 0.0;
        double believed_edge_stakewt = 0.0;
        std::vector<double> pnl_market(M, 0.0);
        std::vector<char> market_bet(M, 0);
        double total_pnl = 0.0, total_stake_settled = 0.0;

        // post-jump accumulators
        uint64_t b_cnt[8] = {0}; double b_stake[8] = {0}, b_edge[8] = {0}, b_pnl[8] = {0}, b_pnlstake[8] = {0};
        std::unordered_map<uint64_t, double> episode_edge;   // (market, last_jump) -> |edge mass|
        double total_edge_mass = 0.0;

        BetRecord br{};
        while (std::fread(&br, sizeof(br), 1, bf) == 1) {
            ++n_bets;
            const double o_take = br.odds_milli_taken / 1000.0;
            total_stake += br.stake;
            const double edge = br.fair_prob * o_take - 1.0;
            const double edge_mass = br.stake * edge;
            believed_edge_stakewt += edge_mass;

            if (br.market_id < M) {
                const uint32_t cb = closing_best[br.market_id][br.outcome];
                if (cb >= 1001) {
                    const double o_close = cb / 1000.0;
                    const double clv = o_take / o_close - 1.0;
                    clv_sum += clv; clv_stake_sum += br.stake * clv;
                    stake_for_clv += br.stake; ++n_clv;
                    if (o_take > o_close) { ++beat; beat_stake += br.stake; }
                }
                double pnl = 0.0; bool settled = false;
                if (have_settle && winner[br.market_id] >= 0) {
                    market_bet[br.market_id] = 1;
                    pnl = (winner[br.market_id] == br.outcome) ? br.stake * (o_take - 1.0) : -br.stake;
                    pnl_market[br.market_id] += pnl;
                    total_pnl += pnl; total_stake_settled += br.stake; settled = true;
                }
                if (have_jumps) {
                    const uint64_t tick = br.sim_timestamp_ns / kTickIntervalNs;
                    const int64_t tsj = ticks_since_jump(jump_ticks[br.market_id], tick);
                    const int bk = bucket_of(tsj);
                    ++b_cnt[bk]; b_stake[bk] += br.stake; b_edge[bk] += edge_mass;
                    if (settled) { b_pnl[bk] += pnl; b_pnlstake[bk] += br.stake; }
                    total_edge_mass += edge_mass;
                    // episode = (market, index of last jump). -1 -> baseline episode.
                    int64_t last_jump = -1;
                    if (tsj >= 0) last_jump = static_cast<int64_t>(tick) - tsj;
                    const uint64_t key = (static_cast<uint64_t>(br.market_id) << 40)
                                         ^ static_cast<uint64_t>(last_jump + 1);
                    episode_edge[key] += std::fabs(edge_mass);
                }
            }
        }
        std::fclose(bf);

        std::printf("== bet log ==\n");
        std::printf("bets logged        : %llu\n", (unsigned long long)n_bets);
        std::printf("total stake         : %.4f (bankroll units summed over all bets)\n", total_stake);
        std::printf("mean believed edge  : %.4f%% (stake-weighted, engine's own view)\n",
                    n_bets ? 100.0 * believed_edge_stakewt / total_stake : 0.0);

        std::printf("\n== closing-line value (scores every bet; transferable to real data) ==\n");
        if (n_clv) {
            std::printf("bets with a close   : %llu\n", (unsigned long long)n_clv);
            std::printf("mean CLV            : %+.4f%% (unweighted)\n", 100.0 * clv_sum / n_clv);
            std::printf("mean CLV            : %+.4f%% (stake-weighted)\n", 100.0 * clv_stake_sum / stake_for_clv);
            std::printf("beat the close      : %.2f%% of bets, %.2f%% of stake\n",
                        100.0 * beat / n_clv, 100.0 * beat_stake / stake_for_clv);
        } else std::printf("no bets had a comparable closing line\n");

        if (have_settle) {
            size_t events = 0; double sum_abs = 0.0, sum_sq = 0.0; std::vector<double> absv;
            for (size_t m = 0; m < M; ++m) if (market_bet[m]) {
                ++events; const double a = std::fabs(pnl_market[m]); sum_abs += a; absv.push_back(a);
            }
            for (double a : absv) sum_sq += a * a;
            double herf = (sum_abs > 0.0) ? sum_sq / (sum_abs * sum_abs) : 0.0;
            double eff_events = (herf > 0.0) ? 1.0 / herf : 0.0;
            std::printf("\n== realised PnL (needs settlement) ==\n");
            std::printf("settled markets bet : %zu events\n", events);
            std::printf("total PnL           : %+.4f bankroll units\n", total_pnl);
            std::printf("ROI on stake        : %+.4f%% (realised)\n",
                        total_stake_settled > 0 ? 100.0 * total_pnl / total_stake_settled : 0.0);
            std::printf("effective # markets : %.1f  (1 / Herfindahl of per-market |PnL|)\n", eff_events);
        }

        if (have_jumps) {
            std::printf("\n== post-jump edge profile (docs Q4) ==\n");
            std::printf("%-12s %12s %8s %12s %12s\n", "since-jump", "bets", "%bets", "believ.edge", "edge-share");
            for (int bk = 0; bk < 8; ++bk) {
                if (b_cnt[bk] == 0) continue;
                std::printf("%-12s %12llu %7.2f%% %11.3f%% %11.2f%%\n",
                            kBucketLabel[bk], (unsigned long long)b_cnt[bk],
                            100.0 * b_cnt[bk] / n_bets,
                            b_stake[bk] > 0 ? 100.0 * b_edge[bk] / b_stake[bk] : 0.0,
                            total_edge_mass != 0.0 ? 100.0 * b_edge[bk] / total_edge_mass : 0.0);
            }
            if (have_settle) {
                std::printf("\nrealised ROI by bucket:\n");
                for (int bk = 0; bk < 8; ++bk) {
                    if (b_pnlstake[bk] <= 0) continue;
                    std::printf("  %-12s ROI %+.3f%%  PnL %+.3f\n", kBucketLabel[bk],
                                100.0 * b_pnl[bk] / b_pnlstake[bk], b_pnl[bk]);
                }
            }
            // Effective sample size for the EDGE signal, at jump-episode granularity.
            double se = 0.0, se2 = 0.0;
            for (const auto& kv : episode_edge) { se += kv.second; se2 += kv.second * kv.second; }
            double herf_e = (se > 0.0) ? se2 / (se * se) : 0.0;
            double eff_ep = (herf_e > 0.0) ? 1.0 / herf_e : 0.0;
            std::printf("\njump episodes bet   : %zu distinct (market, last-jump) windows\n", episode_edge.size());
            std::printf("bets per episode    : %.1f\n", episode_edge.empty() ? 0.0 : (double)n_bets / episode_edge.size());
            std::printf("effective # signals : %.1f  (1 / Herfindahl of per-episode edge mass)\n", eff_ep);
            std::printf("\nQ4: %llu bets rest on ~%.0f effective independent signals. Any edge or\n"
                        "Sharpe estimate must be scored against that number, not the bet count.\n",
                        (unsigned long long)n_bets, eff_ep);
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
