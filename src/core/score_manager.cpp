#include "score_manager.h"

#include <algorithm>
#include <mutex>
#include <stdexcept>

namespace healthec::core {

ScoreManager::ScoreManager(ScoreParams params) : params_(params) {
    if (params_.alpha_H <= 0.0 || params_.alpha_H > 1.0 ||
        params_.alpha_S <= 0.0 || params_.alpha_S > 1.0 ||
        params_.alpha_D <= 0.0 || params_.alpha_D > 1.0) {
        throw std::invalid_argument("EMA coefficients must be in (0, 1]");
    }
    if (params_.alpha_D >= params_.alpha_S) {
        throw std::invalid_argument("alpha_D must be less than alpha_S");
    }
}

// ── H_d ──────────────────────────────────────────────────────────────────────

double& ScoreManager::health_entry(DiskId disk) {
    auto [it, inserted] = health_scores_.emplace(disk, 1.0);
    return it->second;
}

void ScoreManager::update_health(DiskId disk, double l_d, double q_d, double e_d) {
    // Aggregation: f → 1 when disk is healthy, f → 0 when fully degraded.
    double f = params_.w_latency * (1.0 - l_d)
             + params_.w_queue   * (1.0 - q_d)
             + params_.w_error   * (1.0 - e_d);

    std::unique_lock lock(mu_);
    double& h = health_entry(disk);
    h = (1.0 - params_.alpha_H) * h + params_.alpha_H * f;
}

double ScoreManager::get_health(DiskId disk) const {
    std::shared_lock lock(mu_);
    auto it = health_scores_.find(disk);
    return (it != health_scores_.end()) ? it->second : 1.0;
}

std::vector<DiskId> ScoreManager::get_all_disks_sorted_by_health() const {
    std::shared_lock lock(mu_);
    std::vector<DiskId> disks;
    disks.reserve(health_scores_.size());
    for (auto& [id, _] : health_scores_)
        disks.push_back(id);
    std::sort(disks.begin(), disks.end(), [this](DiskId a, DiskId b) {
        return health_scores_.at(a) > health_scores_.at(b);
    });
    return disks;
}

// ── S_i ──────────────────────────────────────────────────────────────────────

void ScoreManager::update_slowness(ShardId shard, double w_s, double event) {
    std::unique_lock lock(mu_);
    auto [it, inserted] = slowness_scores_.emplace(shard, 0.0);
    double& s = it->second;
    s = (1.0 - params_.alpha_S) * s + params_.alpha_S * w_s * event;
}

double ScoreManager::get_slowness(ShardId shard) const {
    std::shared_lock lock(mu_);
    auto it = slowness_scores_.find(shard);
    return (it != slowness_scores_.end()) ? it->second : 0.0;
}

bool ScoreManager::exceeds_slowness_threshold(ShardId shard) const {
    return get_slowness(shard) > params_.theta_S;
}

// ── D_i ──────────────────────────────────────────────────────────────────────

void ScoreManager::update_death(ShardId shard, double w_s, double parity_win_event) {
    std::unique_lock lock(mu_);
    auto [it, inserted] = death_scores_.emplace(shard, 0.0);
    double& d = it->second;
    d = (1.0 - params_.alpha_D) * d + params_.alpha_D * w_s * parity_win_event;
}

double ScoreManager::get_death(ShardId shard) const {
    std::shared_lock lock(mu_);
    auto it = death_scores_.find(shard);
    return (it != death_scores_.end()) ? it->second : 0.0;
}

bool ScoreManager::exceeds_death_threshold(ShardId shard) const {
    return get_death(shard) > params_.theta_D;
}

// ── reset ─────────────────────────────────────────────────────────────────────

void ScoreManager::reset(ShardId shard) {
    std::unique_lock lock(mu_);
    slowness_scores_[shard] = 0.0;
    death_scores_[shard]    = 0.0;
}

}  // namespace healthec::core
