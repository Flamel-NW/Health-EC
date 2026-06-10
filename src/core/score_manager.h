#pragma once

#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace healthec::core {

using DiskId  = int;
using ShardId = int;

// Parameters for the three-score system.
// All thresholds are placeholder defaults; they must be tuned experimentally
// (see design/scoring-algorithms.md §6 — "thresholds must not be hard-coded").
struct ScoreParams {
    // EMA coefficients — α_S > α_D so S_i tracks short-term fluctuations
    // while D_i captures long-term trends.
    double alpha_H = 0.1;   // Health Score EMA coefficient
    double alpha_S = 0.2;   // Slowness Score EMA coefficient
    double alpha_D = 0.05;  // Death Score EMA coefficient

    // Trigger thresholds (placeholder; set via experiments in T2).
    // Calibrated values: see Health-EC-workflow/design/evaluation-params.md T2.2.2.
    double theta_S = 0.6;   // S_i threshold for proactive parity read
    double theta_D = 0.7;   // D_i threshold for shard migration candidacy

    // loser_event significance gate (Phase A, normal read).
    // Both conditions must hold; 0.0 defaults → always significant (backwards-compatible).
    //   loser_lat >= median * (1 + loser_sig_ratio)  AND
    //   loser_lat >= median + loser_sig_abs_ms
    double loser_sig_ratio  = 0.0;
    double loser_sig_abs_ms = 0.0;

    // parity_win_event significance gate (Phase B, proactive read).
    // parity_win_event = 1 only when shard_lat - parity_lat > parity_win_abs_ms.
    // Default 0.0 → any parity win counts (original behaviour, backwards-compatible).
    double parity_win_abs_ms = 0.0;

    // Weights for Health Score aggregation:
    //   f(l, q, e) = w_latency*(1-l) + w_queue*(1-q) + w_error*(1-e)
    // Inputs are normalised to [0,1]; lower means healthier.
    // Weights must sum to 1.0.
    double w_latency = 0.5;
    double w_queue   = 0.3;
    double w_error   = 0.2;
};

// Central store for the three per-disk / per-shard health scores used by the
// Health-EC mitigation framework.
//
// Thread-safety: all public methods are safe to call concurrently.  Reads
// acquire a shared lock; writes acquire an exclusive lock.
class ScoreManager {
public:
    explicit ScoreManager(ScoreParams params = ScoreParams{});

    // ------------------------------------------------------------------ H_d --
    // Update disk d's Health Score via EMA.
    //   l_d : normalised recent I/O latency      [0,1], lower = healthier
    //   q_d : normalised queue depth             [0,1], lower = healthier
    //   e_d : normalised error rate              [0,1], lower = healthier
    // Unregistered disks start at H_d = 1.0 (perfectly healthy).
    void   update_health(DiskId disk, double l_d, double q_d, double e_d);
    double get_health(DiskId disk) const;

    // Returns all registered disk IDs sorted by H_d descending.
    // Used by PlacementPolicy to pick the top k+m disks for a new stripe.
    std::vector<DiskId> get_all_disks_sorted_by_health() const;

    // ------------------------------------------------------------------ S_i --
    // Update shard i's Slowness Score via hot-weighted EMA.
    //   w_s   : stripe hotness weight (≥ 0; 1.0 = average-hot stripe)
    //   event : loser_event (phase A) or parity_win_event (phase B) ∈ {0.0, 1.0}
    // Unregistered shards start at S_i = 0.0.
    void   update_slowness(ShardId shard, double w_s, double event);
    double get_slowness(ShardId shard) const;
    bool   exceeds_slowness_threshold(ShardId shard) const;

    // ------------------------------------------------------------------ D_i --
    // Update shard i's Death Score via hot-weighted EMA (slower decay than S_i).
    // Should be called only when in proactive-parity-read mode.
    //   w_s              : stripe hotness weight
    //   parity_win_event : 1.0 if parity beat shard i and entered final k reads
    void   update_death(ShardId shard, double w_s, double parity_win_event);
    double get_death(ShardId shard) const;
    bool   exceeds_death_threshold(ShardId shard) const;

    // Reset S_i and D_i to 0 after shard migration completes.
    void reset(ShardId shard);

    const ScoreParams& params() const noexcept { return params_; }

private:
    ScoreParams params_;
    mutable std::shared_mutex mu_;

    std::unordered_map<DiskId,  double> health_scores_;    // H_d, default 1.0
    std::unordered_map<ShardId, double> slowness_scores_;  // S_i, default 0.0
    std::unordered_map<ShardId, double> death_scores_;     // D_i, default 0.0

    // Returns H_d for disk, initialising to 1.0 if absent.
    // Caller must hold a write lock when calling this.
    double& health_entry(DiskId disk);
};

}  // namespace healthec::core
