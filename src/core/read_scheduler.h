#pragma once

#include "score_manager.h"

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace healthec::core {

// ── Shared pure functions (also used by calibration tools) ────────────────────

// Identify the "significant loser" among data shards in a normal (Phase A) read.
// data_lat: (ShardId, latency_ms) pairs for all k data shards in this read.
// sig_ratio, sig_abs_ms: loser must satisfy BOTH:
//   loser_lat >= median * (1 + sig_ratio)  AND  loser_lat >= median + sig_abs_ms
// With defaults 0.0/0.0 the loser is always the argmax (backwards-compatible).
// Returns nullopt when no shard is significant (all too close to median).
inline std::optional<ShardId> compute_loser_significant(
    const std::vector<std::pair<ShardId, double>>& data_lat,
    double sig_ratio, double sig_abs_ms)
{
    if (data_lat.empty()) return std::nullopt;

    // Find argmax latency.
    ShardId loser_id  = data_lat[0].first;
    double  loser_lat = data_lat[0].second;
    for (std::size_t i = 1; i < data_lat.size(); ++i) {
        if (data_lat[i].second > loser_lat) {
            loser_lat = data_lat[i].second;
            loser_id  = data_lat[i].first;
        }
    }

    // Compute median with linear interpolation (same semantics as calibration percentile).
    std::vector<double> sorted;
    sorted.reserve(data_lat.size());
    for (auto& kv : data_lat) sorted.push_back(kv.second);
    std::sort(sorted.begin(), sorted.end());

    double midx = 0.5 * static_cast<double>(sorted.size() - 1);
    std::size_t lo = static_cast<std::size_t>(midx);
    std::size_t hi = lo + 1;
    double median;
    if (hi >= sorted.size()) {
        median = sorted.back();
    } else {
        double frac = midx - static_cast<double>(lo);
        median = sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
    }

    if (loser_lat >= median * (1.0 + sig_ratio) && loser_lat >= median + sig_abs_ms)
        return loser_id;
    return std::nullopt;
}

// Phase B helper: did the parity shard beat this data shard by a significant margin?
// abs_ms=0.0 → any parity win counts (parity_lat < shard_lat); backwards-compatible.
// abs_ms>0.0 → parity must be faster by more than abs_ms ms (filters close races on
//   normal-disk shards where parity ≈ shard latency, breaking the Phase B feedback loop).
inline bool parity_won(double parity_lat, double shard_lat, double abs_ms = 0.0) {
    return (shard_lat - parity_lat) > abs_ms;
}

// ─────────────────────────────────────────────────────────────────────────────

using StripeId = int;

// Result of reading a single shard from disk.
struct ShardReadResult {
    ShardId shard_id;
    bool    is_parity;    // true if this was a parity shard
    double  latency_ms;   // wall-clock read latency in milliseconds
};

// Aggregated result of one stripe read.
struct StripeReadResult {
    StripeId stripe_id;
    bool     proactive_mode;                     // true → k+1 reads were issued
    std::vector<ShardReadResult> completed;      // the k results that counted
    std::vector<ShardId>        stragglers;      // shards that lost the race
};

// Physical layout of a single stripe: which shards live on which disks.
struct StripeLayout {
    std::vector<ShardId>               data_shards;    // k data shards in order
    std::vector<ShardId>               parity_shards;  // m parity shards in order
    std::unordered_map<ShardId, DiskId> disk_of;       // shard → hosting disk
};

// Injectable I/O callback — blocks until the shard read completes.
// Tests supply a lambda; T1.4 DiskSimulator will implement the real version.
using ShardReader =
    std::function<ShardReadResult(DiskId disk, ShardId shard, bool is_parity)>;

// Schedules EC stripe reads with proactive parity read support.
//
// When any data shard in the stripe has S_i > θ_S, ReadScheduler issues
// k+1 parallel reads (all k data shards + the healthiest parity shard) and
// returns after the first k complete (first-k-complete).  Score updates
// (S_i and D_i) are dispatched asynchronously so they do not stall the read.
//
// Thread-safety: read_stripe() may be called concurrently from multiple
// threads; each call manages its own shared state.
class ReadScheduler {
public:
    // score_manager must outlive this object.
    ReadScheduler(int k, int m, ScoreManager& score_manager, ShardReader reader);

    // Read stripe sid, returning when the first k reads succeed.
    //   stripe_hotness : w_s for score updates (1.0 = average-hot stripe).
    StripeReadResult read_stripe(StripeId sid, const StripeLayout& layout,
                                 double stripe_hotness = 1.0);

    // Block until all pending async score updates (and straggler read futures)
    // have finished.  Call this in tests before inspecting ScoreManager state.
    void flush_score_updates();

private:
    int           k_;
    int           m_;
    ScoreManager& score_manager_;
    ShardReader   reader_;

    std::mutex                     futures_mu_;
    std::vector<std::future<void>> pending_futures_;

    // Select the parity shard whose hosting disk has the highest H_d.
    ShardId pick_best_parity(const StripeLayout& layout) const;

    // Compute and apply S_i / D_i updates based on the stripe read.
    // Phase A (normal): loser = max-latency data shard → update_slowness only.
    // Phase B (proactive): parity_win_event per shard → update_slowness + update_death.
    void update_scores(const StripeReadResult& result, const StripeLayout& layout,
                       double stripe_hotness,
                       const std::vector<ShardReadResult>& all_results);
};

}  // namespace healthec::core
