#pragma once

#include "score_manager.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace healthec::core {

// Callback that physically moves shard `shard` from disk `src` to disk `dst`.
// Provided by DiskSimulator::make_mover(); injected to decouple core from sim.
using ShardMover = std::function<void(DiskId src, ShardId shard, DiskId dst)>;

// Entry in the migration candidate queue.
struct MigrationCandidate {
    ShardId shard_id;
    DiskId  current_disk;
    double  death_score;   // D_i at enqueue time; updated by re-enqueue
};

// Tunable knobs for MigrationScheduler.
// Concrete values must be determined experimentally (T2; see scoring-algorithms.md §6).
struct MigrationParams {
    int    budget_B = 4;     // max shards migrated per background tick
    double tick_ms  = 100.0; // interval between migration ticks (ms)
};

// Background shard migration scheduler driven by Death Score D_i.
//
// Lifecycle:
//   1. Construct with a ScoreManager reference and a ShardMover callback.
//   2. Call start() to launch the background thread.
//   3. Call enqueue() (from any thread) when D_i > theta_D.
//   4. Call stop() to gracefully drain and join the thread.
//
// Per-tick behaviour (see tick_once()):
//   - Scan candidates ordered by D_i descending until budget_B migrations succeed.
//   - For each candidate, find the healthiest disk d' satisfying
//       H_{d'} > H_{current_disk}  &&  d' != current_disk.
//   - If a valid target exists: invoke mover_, then ScoreManager::reset().
//   - If no valid target exists: leave the candidate in the queue.
//   - If mover_ fails: reset and remove that candidate so it cannot starve
//     lower-scored candidates on later ticks.
//
// Thread-safety: all public methods are safe to call concurrently.
class MigrationScheduler {
public:
    MigrationScheduler(ScoreManager&   sm,
                       ShardMover      mover,
                       MigrationParams params = MigrationParams{});
    ~MigrationScheduler();

    // Submit shard as a migration candidate.
    // If the shard is already queued, updates its death_score and current_disk
    // to the supplied values (upsert semantics).
    void enqueue(ShardId shard, DiskId current_disk, double death_score);

    // Start the background migration thread.  Must not be called more than once
    // without an intervening stop().
    void start();

    // Signal the background thread to stop and block until it exits.
    void stop();

    // Execute one migration tick synchronously in the calling thread.
    // Intended for deterministic unit tests; do not call while the background
    // thread is running.
    void tick_once();

private:
    void run();

    // Extract up to n candidates with the highest death_score.
    // Caller must NOT hold mu_.
    std::vector<MigrationCandidate> top_candidates(int n);

    ScoreManager&   sm_;
    ShardMover      mover_;
    MigrationParams params_;

    std::mutex              mu_;
    std::condition_variable cv_;
    std::unordered_map<ShardId, MigrationCandidate> queue_;

    std::atomic<bool> running_{false};
    std::thread       thread_;
};

}  // namespace healthec::core
