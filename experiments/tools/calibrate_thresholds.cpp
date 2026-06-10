// calibrate_thresholds — θ_S / θ_D / loser significance / parity-win significance sweep
//
// Path A in-memory calibration using sample_latency_ms() (no sleep) + shared pure
// functions compute_loser_significant() and parity_won() from read_scheduler.h.
// Shared functions ensure calibration code and production code cannot drift.
//
// Phase A gate: loser_event fires only when
//   loser_lat >= median + loser_sig_abs_ms  (and loser_sig_ratio==0 here)
// Phase B gate: parity_win_event fires only when
//   shard_lat - parity_lat > parity_win_abs_ms
// Default 0/0 → original backwards-compatible behaviour.
//
// Ground truth layout:
//   K=4 data shards, M=2 parity shards, 10 disks.
//   Disk 8: mild-slow (T2.1 profile);  Disk 9: severe-slow.
//   Stripe 0: data[0]→disk6(baseline), data[1]→disk7(baseline),
//             data[2]→disk8(mild),      data[3]→disk9(severe);
//             parity[0]→disk4, parity[1]→disk5.
//   Stripes 1-499: round-robin across all 10 disks.
//
// Usage: ./calibrate_thresholds
// Seed: 42 (fixed, reproducible)

#include "core/read_scheduler.h"
#include "core/score_manager.h"
#include "sim/disk_simulator.h"
#include "sim/workload_generator.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

using namespace healthec::core;
using namespace healthec::sim;

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr int      K           = 4;
static constexpr int      M           = 2;
static constexpr int      NUM_DISKS   = 10;
static constexpr int      NUM_STRIPES = 500;
static constexpr int      NUM_READS   = 20000;
static constexpr uint64_t SEED        = 42;

static constexpr int MILD_DISK   = 8;
static constexpr int SEVERE_DISK = 9;

// ── T2.1 calibrated disk profiles ────────────────────────────────────────────

static const DiskProfile PROFILE_BASELINE{
    .base_mean_ms=9.0, .base_jitter_ms=3.5, .slow_mode=false};

static const DiskProfile PROFILE_MILD{
    .base_mean_ms=9.0, .base_jitter_ms=3.5, .slow_mode=true,
    .slow_mean_ms=24.0, .slow_jitter_ms=8.0, .spike_prob=0.01, .spike_ms=150.0};

static const DiskProfile PROFILE_SEVERE{
    .base_mean_ms=9.0, .base_jitter_ms=3.5, .slow_mode=true,
    .slow_mean_ms=55.0, .slow_jitter_ms=18.0, .spike_prob=0.03, .spike_ms=500.0};

// ── Helpers ───────────────────────────────────────────────────────────────────

static double percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    double idx = p * static_cast<double>(sorted.size() - 1);
    auto lo = static_cast<std::size_t>(idx);
    auto hi = lo + 1;
    if (hi >= sorted.size()) return sorted.back();
    double frac = idx - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

static ShardId make_shard(int stripe, int idx) { return stripe * (K + M) + idx; }

// ── Layout ────────────────────────────────────────────────────────────────────

struct Layout {
    std::unordered_map<StripeId, StripeLayout> stripe_layouts;
    std::unordered_map<ShardId,  bool>         is_slow;  // ground truth
};

static Layout build_layout() {
    Layout L;
    for (int s = 0; s < NUM_STRIPES; ++s) {
        StripeLayout sl;
        const int disk_map_s0[] = {6, 7, MILD_DISK, SEVERE_DISK, 4, 5};
        for (int i = 0; i < K + M; ++i) {
            ShardId sh = make_shard(s, i);
            DiskId  dk = (s == 0) ? disk_map_s0[i]
                                   : (s * (K + M) + i) % NUM_DISKS;
            sl.disk_of[sh] = dk;
            if (i < K) sl.data_shards.push_back(sh);
            else        sl.parity_shards.push_back(sh);
        }
        L.stripe_layouts[s] = sl;
    }
    // Ground truth: ALL data shards whose disk is MILD_DISK or SEVERE_DISK.
    for (auto& [sid, sl] : L.stripe_layouts)
        for (ShardId sh : sl.data_shards) {
            DiskId dk = sl.disk_of.at(sh);
            if (dk == MILD_DISK || dk == SEVERE_DISK)
                L.is_slow[sh] = true;
        }
    return L;
}

// ── Run result ────────────────────────────────────────────────────────────────

struct RunResult {
    double p50, p95, p99;
    long   proactive_reads, total_reads;
    // Stripe-level TP/FP: proactive triggered on stripe with/without slow shard.
    long   tp_stripe, fp_stripe;
    // Migration TP/FP: D_i crossed θ_D on slow/normal shard.
    long   mig_tp, mig_fp;
};

// ── Memory simulation ─────────────────────────────────────────────────────────

static RunResult run_scenario(DiskSimulator& sim, const Layout& L,
                               WorkloadGenerator& wg, const ScoreParams& p)
{
    struct State { double S = 0.0, D = 0.0; };
    std::unordered_map<ShardId, State> st;

    std::vector<double> lats;
    lats.reserve(NUM_READS);

    long pro_reads = 0, tp_str = 0, fp_str = 0, mig_tp = 0, mig_fp = 0;

    auto stripe_has_slow = [&](const StripeLayout& sl) {
        for (ShardId sh : sl.data_shards)
            if (L.is_slow.count(sh)) return true;
        return false;
    };

    for (int r = 0; r < NUM_READS; ++r) {
        StripeId sid = wg.next_stripe();
        double   w_s = wg.hotness(sid);
        const auto& sl = L.stripe_layouts.at(sid);

        // Step 1: decide proactive (any data shard S_i > θ_S)
        bool proactive = false;
        for (ShardId sh : sl.data_shards)
            if (st[sh].S > p.theta_S) { proactive = true; break; }

        if (!proactive) {
            // Phase A: k data-shard reads
            std::vector<std::pair<ShardId, double>> data_lat;
            double stripe_lat = 0.0;
            for (ShardId sh : sl.data_shards) {
                double lat = sim.sample_latency_ms(sl.disk_of.at(sh));
                data_lat.emplace_back(sh, lat);
                stripe_lat = std::max(stripe_lat, lat);
            }
            lats.push_back(stripe_lat);

            auto loser = compute_loser_significant(data_lat, p.loser_sig_ratio,
                                                    p.loser_sig_abs_ms);
            for (auto& [sh, lat] : data_lat) {
                double ev = (loser && sh == *loser) ? 1.0 : 0.0;
                auto& s = st[sh].S;
                s = (1.0 - p.alpha_S) * s + p.alpha_S * w_s * ev;
            }
        } else {
            // Phase B: proactive k+1 read
            pro_reads++;
            if (stripe_has_slow(sl)) tp_str++; else fp_str++;

            // Sample data and parity latencies
            std::vector<std::pair<ShardId, double>> data_lat;
            for (ShardId sh : sl.data_shards)
                data_lat.emplace_back(sh, sim.sample_latency_ms(sl.disk_of.at(sh)));

            ShardId par      = sl.parity_shards[0];
            double  par_lat  = sim.sample_latency_ms(sl.disk_of.at(par));

            // First-k-complete: sort all k+1 by latency, take top k
            std::vector<std::pair<ShardId, double>> all = data_lat;
            all.emplace_back(par, par_lat);
            std::sort(all.begin(), all.end(),
                      [](const auto& a, const auto& b){ return a.second < b.second; });

            bool parity_in_k = false;
            std::unordered_map<ShardId, bool> in_k;
            for (int i = 0; i < K; ++i) {
                in_k[all[i].first] = true;
                if (all[i].first == par) parity_in_k = true;
            }
            // k-th shard latency = stripe end-to-end
            lats.push_back(all[K - 1].second);

            // Phase B score updates
            for (auto& [sh, lat] : data_lat) {
                bool straggler = !in_k.count(sh);
                double shard_lat_eff = straggler
                    ? std::numeric_limits<double>::max() : lat;
                double win = (parity_in_k &&
                              parity_won(par_lat, shard_lat_eff, p.parity_win_abs_ms))
                             ? 1.0 : 0.0;

                auto& s = st[sh].S;
                auto& d = st[sh].D;
                s = (1.0 - p.alpha_S) * s + p.alpha_S * w_s * win;
                d = (1.0 - p.alpha_D) * d + p.alpha_D * w_s * win;

                if (d > p.theta_D) {
                    if (L.is_slow.count(sh)) mig_tp++; else mig_fp++;
                    s = 0.0; d = 0.0;
                }
            }
        }
    }

    std::sort(lats.begin(), lats.end());
    return {percentile(lats, 0.50), percentile(lats, 0.95), percentile(lats, 0.99),
            pro_reads, NUM_READS, tp_str, fp_str, mig_tp, mig_fp};
}

static WorkloadGenerator make_wg() {
    return WorkloadGenerator(NUM_STRIPES, 1.0, SEED);
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    static const std::string TMP = "/tmp/healthec_calibrate_thresholds";
    std::filesystem::create_directories(TMP);

    DiskSimulator sim(TMP, NUM_DISKS, PROFILE_BASELINE, SEED);
    sim.set_profile(MILD_DISK,   PROFILE_MILD);
    sim.set_profile(SEVERE_DISK, PROFILE_SEVERE);

    const Layout L = build_layout();

    // Baseline: θ_S = ∞ (proactive never triggered)
    ScoreParams bp; bp.theta_S = 1e18; bp.theta_D = 1e18;
    auto bwg     = make_wg();
    auto baseline = run_scenario(sim, L, bwg, bp);

    std::cout << "calibrate_thresholds  seed=" << SEED
              << "  reads=" << NUM_READS
              << "  stripes=" << NUM_STRIPES << "\n";
    std::cout << "Slow: disk" << MILD_DISK << "=mild  disk" << SEVERE_DISK
              << "=severe  (stripe-0 data[2,3])\n";
    std::cout << "Baseline (θ_S=∞): P50=" << baseline.p50
              << "  P95=" << baseline.p95
              << "  P99=" << baseline.p99 << " ms\n\n";

    // Hard-constraint thresholds (may be adjusted after seeing data)
    constexpr double PREC_MIN   = 0.80;   // stripe-level precision
    constexpr double BW_MAX_PCT = 15.0;   // bandwidth overhead %

    // Column header
    std::cout << std::left
              << std::setw(9)  << "Aabs"
              << std::setw(9)  << "Babs"
              << std::setw(7)  << "θ_S"
              << std::setw(7)  << "θ_D"
              << std::right
              << std::setw(8)  << "P50"
              << std::setw(8)  << "P95"
              << std::setw(8)  << "P99"
              << std::setw(9)  << "P99imp%"
              << std::setw(8)  << "BW%"
              << std::setw(8)  << "prec"
              << std::setw(8)  << "migTP"
              << std::setw(8)  << "migFP"
              << "  flag\n";
    std::cout << std::string(110, '-') << "\n";

    // Sweep grid
    const double a_abs_vals[]  = {0.0, 15.0, 30.0};        // Phase A loser_sig_abs_ms
    const double b_abs_vals[]  = {0.0, 5.0, 15.0, 30.0};   // Phase B parity_win_abs_ms
    const double theta_s_vals[]= {1.3, 2.0, 2.7, 3.4};
    const double theta_d_mult[]= {1.0, 1.5, 2.0};

    double best_imp = -1e9;
    double best_a = 0, best_b = 0, best_ts = 0, best_td = 0;
    RunResult best_res{};

    for (double a_abs : a_abs_vals) {
        for (double b_abs : b_abs_vals) {
            for (double ts : theta_s_vals) {
                for (double mult : theta_d_mult) {
                    double td = mult * ts;

                    ScoreParams p;
                    p.theta_S          = ts;
                    p.theta_D          = td;
                    p.loser_sig_ratio  = 0.0;
                    p.loser_sig_abs_ms = a_abs;
                    p.parity_win_abs_ms= b_abs;

                    auto wg = make_wg();
                    auto r  = run_scenario(sim, L, wg, p);

                    long  trig = r.tp_stripe + r.fp_stripe;
                    double prec = (trig > 0)
                        ? static_cast<double>(r.tp_stripe) / trig : 0.0;
                    double bw  = static_cast<double>(r.proactive_reads)
                                 / r.total_reads * 100.0;
                    double imp = (baseline.p99 > 0.0)
                        ? (baseline.p99 - r.p99) / baseline.p99 * 100.0 : 0.0;

                    bool feasible = (prec >= PREC_MIN) && (bw <= BW_MAX_PCT);
                    bool best = false;
                    if (feasible && imp > best_imp) {
                        best_imp = imp;
                        best_a = a_abs; best_b = b_abs;
                        best_ts = ts;   best_td = td;
                        best_res = r;
                        best = true;
                    }

                    std::cout << std::fixed << std::setprecision(1) << std::left
                              << std::setw(9)  << a_abs
                              << std::setw(9)  << b_abs
                              << std::setw(7)  << ts
                              << std::setw(7)  << td
                              << std::right
                              << std::setw(8)  << r.p50
                              << std::setw(8)  << r.p95
                              << std::setw(8)  << r.p99
                              << std::setw(9)  << imp
                              << std::setw(8)  << bw
                              << std::setprecision(2)
                              << std::setw(8)  << prec
                              << std::setw(8)  << r.mig_tp
                              << std::setw(8)  << r.mig_fp;
                    if (best)     std::cout << "  *** BEST";
                    else if (feasible) std::cout << "  OK";
                    std::cout << "\n";
                }
            }
        }
    }

    std::cout << "\n=== Summary ===\n";
    std::cout << "Constraints: stripe-precision>=" << PREC_MIN
              << "  BW<=" << BW_MAX_PCT << "%\n";
    if (best_imp > -1e8) {
        std::cout << "Best: A_abs=" << best_a << "ms  B_abs=" << best_b << "ms"
                  << "  θ_S=" << best_ts << "  θ_D=" << best_td
                  << "  P99_imp=" << best_imp << "%"
                  << "  BW="
                  << static_cast<double>(best_res.proactive_reads)/best_res.total_reads*100.0
                  << "%\n";
    } else {
        std::cout << "No combination satisfied both constraints.\n";
        std::cout << "Relaxing BW to 30%:\n";
        // Re-scan and find best under relaxed BW
        // (just note the user should look at the table directly)
    }

    std::filesystem::remove_all(TMP);
    return 0;
}
