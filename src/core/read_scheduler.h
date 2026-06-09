#pragma once

#include "score_manager.h"

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace healthec::core {

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

    // Compute and apply S_i / D_i updates based on the completed stripe read.
    // Phase A (normal): loser = max-latency data shard → update_slowness only.
    // Phase B (proactive): parity_win_event per shard → update_slowness + update_death.
    void update_scores(const StripeReadResult& result, const StripeLayout& layout,
                       double stripe_hotness);
};

}  // namespace healthec::core
