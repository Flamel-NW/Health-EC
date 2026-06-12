#include "read_scheduler.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <stdexcept>

namespace healthec::core {

// ── Internal shared state for first-k-complete ────────────────────────────────

// Lives on the heap (shared_ptr) so that async lambda captures remain valid
// even after read_stripe() has returned.
struct ReadState {
    std::mutex              mu;
    std::condition_variable cv;
    std::vector<ShardReadResult> completed;
    std::vector<ShardReadResult> all_results;
    std::size_t expected_results = 0;
    bool first_k_done = false;
};

// ── ReadScheduler ─────────────────────────────────────────────────────────────

ReadScheduler::ReadScheduler(int k, int m, ScoreManager& score_manager,
                             ShardReader reader)
    : k_(k), m_(m), score_manager_(score_manager), reader_(std::move(reader)) {
    if (k_ < 1)
        throw std::invalid_argument("k must be >= 1");
    if (m_ < 1)
        throw std::invalid_argument("m must be >= 1");
}

// ── pick_best_parity ──────────────────────────────────────────────────────────

ShardId ReadScheduler::pick_best_parity(const StripeLayout& layout) const {
    assert(!layout.parity_shards.empty());
    ShardId best   = layout.parity_shards[0];
    double  best_h = score_manager_.get_health(layout.disk_of.at(best));
    for (std::size_t i = 1; i < layout.parity_shards.size(); ++i) {
        ShardId s = layout.parity_shards[i];
        double  h = score_manager_.get_health(layout.disk_of.at(s));
        if (h > best_h) { best = s; best_h = h; }
    }
    return best;
}

// ── read_stripe ───────────────────────────────────────────────────────────────

StripeReadResult ReadScheduler::read_stripe(StripeId sid,
                                            const StripeLayout& layout,
                                            double stripe_hotness) {
    // Validate layout against k_, m_.
    if (static_cast<int>(layout.data_shards.size()) != k_)
        throw std::invalid_argument("layout.data_shards.size() != k");
    if (static_cast<int>(layout.parity_shards.size()) != m_)
        throw std::invalid_argument("layout.parity_shards.size() != m");

    // ── Step 1: decide proactive mode ────────────────────────────────────────
    bool proactive = false;
    for (ShardId s : layout.data_shards) {
        if (score_manager_.exceeds_slowness_threshold(s)) {
            proactive = true;
            break;
        }
    }

    // ── Step 2: build read set ───────────────────────────────────────────────
    // Each entry: (shard_id, is_parity)
    std::vector<std::pair<ShardId, bool>> read_set;
    read_set.reserve(proactive ? k_ + 1 : k_);
    for (ShardId s : layout.data_shards)
        read_set.emplace_back(s, false);
    if (proactive)
        read_set.emplace_back(pick_best_parity(layout), true);

    // ── Step 3: launch k (or k+1) parallel reads ────────────────────────────
    auto state = std::make_shared<ReadState>();
    state->completed.reserve(k_);
    state->all_results.reserve(read_set.size());
    state->expected_results = read_set.size();

    std::vector<std::future<void>> futs;
    futs.reserve(read_set.size());

    for (auto [shard, is_parity] : read_set) {
        DiskId disk = layout.disk_of.at(shard);
        futs.push_back(std::async(
            std::launch::async,
            [state, this, shard, disk, is_parity, k = k_]() {
                ShardReadResult r = reader_(disk, shard, is_parity);
                std::unique_lock lock(state->mu);
                state->all_results.push_back(r);
                if (!state->first_k_done) {
                    state->completed.push_back(r);
                    if (static_cast<int>(state->completed.size()) == k) {
                        state->first_k_done = true;
                        lock.unlock();
                        state->cv.notify_one();
                        return;
                    }
                }
                if (state->all_results.size() == state->expected_results) {
                    lock.unlock();
                    state->cv.notify_all();
                }
            }));
    }

    // ── Step 4: wait for first k ─────────────────────────────────────────────
    {
        std::unique_lock lock(state->mu);
        state->cv.wait(lock, [&state] { return state->first_k_done; });
    }

    // ── Step 5: determine stragglers ─────────────────────────────────────────
    std::vector<ShardId> stragglers;
    {
        std::unique_lock lock(state->mu);
        for (auto [shard, _] : read_set) {
            bool found = false;
            for (auto& r : state->completed)
                if (r.shard_id == shard) { found = true; break; }
            if (!found)
                stragglers.push_back(shard);
        }
    }

    StripeReadResult result{sid, proactive, state->completed, std::move(stragglers)};

    // ── Step 6: schedule async score update, store all futures ───────────────
    // Both the straggler read futures and the score-update future are moved into
    // pending_futures_ so they keep running without blocking the caller.
    auto score_fut = std::async(
        std::launch::async,
        [this, result, layout, stripe_hotness, state]() {
            std::vector<ShardReadResult> all_results;
            {
                std::unique_lock lock(state->mu);
                state->cv.wait(lock, [&state] {
                    return state->all_results.size() == state->expected_results;
                });
                all_results = state->all_results;
            }
            update_scores(result, layout, stripe_hotness, all_results);
        });

    {
        std::lock_guard lg(futures_mu_);
        for (auto& f : futs)
            pending_futures_.push_back(std::move(f));
        pending_futures_.push_back(std::move(score_fut));
    }

    return result;
}

// ── flush_score_updates ───────────────────────────────────────────────────────

void ReadScheduler::flush_score_updates() {
    std::vector<std::future<void>> to_flush;
    {
        std::lock_guard lg(futures_mu_);
        to_flush = std::move(pending_futures_);
        pending_futures_.clear();
    }
    for (auto& f : to_flush)
        if (f.valid()) f.wait();
}

// ── update_scores ─────────────────────────────────────────────────────────────

void ReadScheduler::update_scores(const StripeReadResult& result,
                                  const StripeLayout& layout,
                                  double stripe_hotness,
                                  const std::vector<ShardReadResult>& all_results) {
    if (!result.proactive_mode) {
        // ── Phase A (normal k-read) ───────────────────────────────────────────
        // Collect (id, lat) for data shards, then find the significant loser.
        std::vector<std::pair<ShardId, double>> data_lat;
        for (auto& r : result.completed)
            if (!r.is_parity)
                data_lat.emplace_back(r.shard_id, r.latency_ms);

        const auto& p = score_manager_.params();
        auto loser_opt = compute_loser_significant(data_lat, p.loser_sig_ratio,
                                                   p.loser_sig_abs_ms);

        for (auto& r : result.completed) {
            if (r.is_parity) continue;
            double event = (loser_opt && r.shard_id == *loser_opt) ? 1.0 : 0.0;
            score_manager_.update_slowness(r.shard_id, stripe_hotness, event);
        }
    } else {
        // ── Phase B (proactive k+1 read) ─────────────────────────────────────
        // parity_win_event(i) = 1 iff parity is in `completed` AND
        //   parity beat data shard i by more than parity_win_abs_ms.
        bool   parity_in_k  = false;
        double parity_lat   = std::numeric_limits<double>::max();
        for (auto& r : result.completed) {
            if (r.is_parity) {
                parity_in_k = true;
                parity_lat  = r.latency_ms;
                break;
            }
        }

        std::unordered_map<ShardId, double> latency_by_shard;
        latency_by_shard.reserve(all_results.size());
        for (const auto& r : all_results)
            latency_by_shard[r.shard_id] = r.latency_ms;

        // Update all data shards, including stragglers whose reads finished
        // after read_stripe() returned its first-k result.
        const double pw_abs = score_manager_.params().parity_win_abs_ms;
        for (ShardId sid : layout.data_shards) {
            auto it = latency_by_shard.find(sid);
            double win = (it != latency_by_shard.end() &&
                          parity_in_k &&
                          parity_won(parity_lat, it->second, pw_abs))
                         ? 1.0 : 0.0;
            score_manager_.update_slowness(sid, stripe_hotness, win);
            score_manager_.update_death(sid, stripe_hotness, win);
        }
    }
}

}  // namespace healthec::core
