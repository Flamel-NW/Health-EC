#include "migration_scheduler.h"

#include <algorithm>
#include <chrono>

namespace healthec::core {

MigrationScheduler::MigrationScheduler(ScoreManager&   sm,
                                        ShardMover      mover,
                                        MigrationParams params)
    : sm_(sm), mover_(std::move(mover)), params_(params) {}

MigrationScheduler::~MigrationScheduler() {
    stop();
}

void MigrationScheduler::enqueue(ShardId shard, DiskId current_disk,
                                  double death_score) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_[shard] = MigrationCandidate{shard, current_disk, death_score};
    }
    cv_.notify_one();
}

void MigrationScheduler::start() {
    running_.store(true);
    thread_ = std::thread(&MigrationScheduler::run, this);
}

void MigrationScheduler::stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

// ── tick_once ────────────────────────────────────────────────────────────────

std::vector<MigrationCandidate> MigrationScheduler::top_candidates(int n) {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<MigrationCandidate> all;
    all.reserve(queue_.size());
    for (auto& [id, cand] : queue_) all.push_back(cand);

    int take = std::min(n, static_cast<int>(all.size()));
    // Partial sort: highest death_score first.
    std::partial_sort(all.begin(), all.begin() + take, all.end(),
                      [](const MigrationCandidate& a, const MigrationCandidate& b) {
                          return a.death_score > b.death_score;
                      });
    all.resize(take);
    return all;
}

void MigrationScheduler::tick_once() {
    auto candidates = top_candidates(params_.budget_B);

    for (const auto& cand : candidates) {
        // Find the healthiest disk with H_d strictly greater than current disk.
        auto sorted_disks = sm_.get_all_disks_sorted_by_health();
        double cur_health  = sm_.get_health(cand.current_disk);

        DiskId target = -1;
        for (DiskId d : sorted_disks) {
            if (d == cand.current_disk) continue;
            if (sm_.get_health(d) > cur_health) {
                target = d;
                break;  // sorted descending; first valid candidate is the best
            }
        }

        if (target == -1) continue;  // no valid target; leave in queue

        mover_(cand.current_disk, cand.shard_id, target);
        sm_.reset(cand.shard_id);

        {
            std::lock_guard<std::mutex> lk(mu_);
            queue_.erase(cand.shard_id);
        }
    }
}

// ── Background thread ─────────────────────────────────────────────────────────

void MigrationScheduler::run() {
    while (running_.load()) {
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk,
                         std::chrono::duration<double, std::milli>(params_.tick_ms),
                         [this] { return !running_.load() || !queue_.empty(); });
        }

        if (!running_.load()) break;
        tick_once();
    }
}

}  // namespace healthec::core
