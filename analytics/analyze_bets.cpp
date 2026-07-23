#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>

#include "../proto/messages.hpp"
#include "../engine/SessionReader.hpp"
#include "../engine/BetLog.hpp"
#include "../sim/SettlementSink.hpp"

// Scores a bet log against the session it was produced from.
//
//   Closing-line value (CLV): for each bet, compare the odds it took to the
//   closing best odds for that (market, outcome) - the highest price any book
//   was still quoting on its final update. Beating the close is the standard
//   proxy for a real edge and, crucially for docs Q4, it scores EVERY bet
//   rather than waiting for the market to resolve, so its sample size is the
//   bet count, not the (tiny) event count.
//
//   PnL (optional, needs --settle): realise each market against its drawn
//   winner and attribute profit per event. This is where Q4 bites: PnL is an
//   EVENT-count statistic. The Herfindahl of per-market |PnL| gives an
//   effective number of events actually driving the result, which is what you
//   should trust the Sharpe/edge estimate against, not the bet count.

namespace {
struct Args {
    std::string bets;
    std::string session;
    std::string settle;   // optional
    double bankroll = 1.0;
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
        else { std::fprintf(stderr, "unknown arg: %s\n", s.c_str()); return false; }
    }
    if (a.bets.empty() || a.session.empty()) {
        std::fprintf(stderr, "usage: analyze_bets --bets PATH --session PATH [--settle PATH]\n");
        return false;
    }
    return true;
}
} // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse(argc, argv, args)) return 2;

    try {
        SessionReader reader(args.session);
        const auto& h = reader.header();
        const size_t M = h.markets, B = h.books;

        // Per (market,book) last quote -> closing best odds per (market,outcome).
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

        // Optional settlement.
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


        // Bet log.
        std::FILE* bf = std::fopen(args.bets.c_str(), "rb");
        if (!bf) throw std::runtime_error("cannot open bet log");
        BetLogHeader bh{};
        if (std::fread(&bh, sizeof(bh), 1, bf) != 1) throw std::runtime_error("bet header short");
        if (bh.magic != kBetLogMagic) throw std::runtime_error("bad bet log magic");
        if (bh.record_size != sizeof(BetRecord)) throw std::runtime_error("bet record size mismatch");

        // Accumulators.
        uint64_t n_bets = 0, n_clv = 0, beat = 0;
        double total_stake = 0.0, clv_sum = 0.0, clv_stake_sum = 0.0, stake_for_clv = 0.0, beat_stake = 0.0;
        double believed_edge_stakewt = 0.0;
        std::vector<double> pnl_market(M, 0.0);
        std::vector<char> market_bet(M, 0);
        double total_pnl = 0.0, total_stake_settled = 0.0;

        BetRecord br{};
        while (std::fread(&br, sizeof(br), 1, bf) == 1) {
            ++n_bets;
            const double o_take = br.odds_milli_taken / 1000.0;
            total_stake += br.stake;
            believed_edge_stakewt += br.stake * (br.fair_prob * o_take - 1.0);

            if (br.market_id < M) {
                const uint32_t cb = closing_best[br.market_id][br.outcome];
                if (cb >= 1001) {
                    const double o_close = cb / 1000.0;
                    const double clv = o_take / o_close - 1.0;
                    clv_sum += clv; clv_stake_sum += br.stake * clv;
                    stake_for_clv += br.stake; ++n_clv;
                    if (o_take > o_close) { ++beat; beat_stake += br.stake; }
                }
                if (have_settle && winner[br.market_id] >= 0) {
                    market_bet[br.market_id] = 1;
                    const double pnl = (winner[br.market_id] == br.outcome)
                                       ? br.stake * (o_take - 1.0) : -br.stake;
                    pnl_market[br.market_id] += pnl;
                    total_pnl += pnl; total_stake_settled += br.stake;
                }
            }
        }
        std::fclose(bf);

        std::printf("== bet log ==\n");
        std::printf("bets logged        : %llu\n", (unsigned long long)n_bets);
        std::printf("total stake         : %.4f (bankroll units summed over all bets)\n", total_stake);
        std::printf("mean believed edge  : %.4f%% (stake-weighted, engine's own view)\n",
                    n_bets ? 100.0 * believed_edge_stakewt / total_stake : 0.0);

        std::printf("\n== closing-line value (scores every bet; the Q4-robust metric) ==\n");
        if (n_clv) {
            std::printf("bets with a close   : %llu\n", (unsigned long long)n_clv);
            std::printf("mean CLV            : %+.4f%% (unweighted)\n", 100.0 * clv_sum / n_clv);
            std::printf("mean CLV            : %+.4f%% (stake-weighted)\n", 100.0 * clv_stake_sum / stake_for_clv);
            std::printf("beat the close      : %.2f%% of bets, %.2f%% of stake\n",
                        100.0 * beat / n_clv, 100.0 * beat_stake / stake_for_clv);
        } else {
            std::printf("no bets had a comparable closing line\n");
        }

        if (have_settle) {
            size_t events = 0; double sum_abs = 0.0, sum_sq = 0.0;
            std::vector<double> absv;
            for (size_t m = 0; m < M; ++m) if (market_bet[m]) {
                ++events; const double a = std::fabs(pnl_market[m]);
                sum_abs += a; absv.push_back(a);
            }
            for (double a : absv) sum_sq += a * a;
            double herf = (sum_abs > 0.0) ? sum_sq / (sum_abs * sum_abs) : 0.0;
            double eff_events = (herf > 0.0) ? 1.0 / herf : 0.0;
            std::sort(absv.rbegin(), absv.rend());
            double top10 = 0.0; size_t k = std::max<size_t>(1, absv.size() / 10);
            for (size_t i = 0; i < k && i < absv.size(); ++i) top10 += absv[i];

            std::printf("\n== realised PnL (needs settlement; the Q4-fragile metric) ==\n");
            std::printf("settled markets bet : %zu events\n", events);
            std::printf("bets per event      : %.1f\n", events ? (double)n_bets / events : 0.0);
            std::printf("total PnL           : %+.4f bankroll units\n", total_pnl);
            std::printf("ROI on stake        : %+.4f%% (realised)\n",
                        total_stake_settled > 0 ? 100.0 * total_pnl / total_stake_settled : 0.0);
            std::printf("effective # events  : %.1f  (1 / Herfindahl of per-event |PnL|)\n", eff_events);
            std::printf("top-10%% events hold : %.1f%% of gross |PnL|\n",
                        sum_abs > 0 ? 100.0 * top10 / sum_abs : 0.0);
            std::printf("\nQ4 in one line: %llu bets, but only ~%.0f independent events actually\n"
                        "drive the PnL. Trust the edge/Sharpe estimate against the smaller number.\n",
                        (unsigned long long)n_bets, eff_events);
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
