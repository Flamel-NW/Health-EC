#include "migration_scheduler.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <limits>

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
        queue_[shard] = MigrationCandidate{shard, current_disk, ++next_generation_, death_score};
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
    auto candidates=top_candidates(std::numeric_limits<int>::max());int migrated=0;
    for(const auto& cand:candidates){
        if(migrated>=params_.budget_B)break;
        DiskId target=-1;double current=sm_.get_health(cand.current_disk);
        for(DiskId disk:sm_.get_all_disks_sorted_by_health())
            if(disk!=cand.current_disk && sm_.get_health(disk)>current){target=disk;break;}
        if(target<0)continue;
        auto erase_generation=[&]{
            std::lock_guard<std::mutex> guard(mu_);auto it=queue_.find(cand.shard_id);
            if(it!=queue_.end() && it->second.generation==cand.generation)queue_.erase(it);
        };
        try{mover_(cand.current_disk,cand.shard_id,target);}
        catch(...){erase_generation();continue;}
        sm_.reset(cand.shard_id);++migrated;erase_generation();
    }
}

// ── Background thread ─────────────────────────────────────────────────────────

void MigrationScheduler::run() {
    while (running_.load()) {
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk,
                         std::chrono::duration<double, std::milli>(params_.tick_ms),
                         [this] { return !running_.load(); });
        }

        if (!running_.load()) break;
        try {
            tick_once();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[MigrationScheduler] tick_once exception: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "[MigrationScheduler] tick_once unknown exception\n");
        }
    }
}

}  // namespace healthec::core
