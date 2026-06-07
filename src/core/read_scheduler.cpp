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
    bool done = false;
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

    std::vector<std::future<void>> futs;
    futs.reserve(read_set.size());

    for (auto [shard, is_parity] : read_set) {
        DiskId disk = layout.disk_of.at(shard);
        futs.push_back(std::async(
            std::launch::async,
            [state, this, shard, disk, is_parity, k = k_]() {
                ShardReadResult r = reader_(disk, shard, is_parity);
                std::unique_lock lock(state->mu);
                if (!state->done) {
                    state->completed.push_back(r);
                    if (static_cast<int>(state->completed.size()) == k) {
                        state->done = true;
                        lock.unlock();
                        state->cv.notify_one();
                    }
                }
            }));
    }

    // ── Step 4: wait for first k ─────────────────────────────────────────────
    {
        std::unique_lock lock(state->mu);
        state->cv.wait(lock, [&state] { return state->done; });
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
        [this, result, stripe_hotness]() { update_scores(result, stripe_hotness); });

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
                                  double stripe_hotness) {
    if (!result.proactive_mode) {
        // ── Phase A (normal k-read) ───────────────────────────────────────────
        // loser = the data shard with the maximum latency in `completed`.
        // All k shards are in `completed`; there are no stragglers in normal mode.
        ShardId loser    = -1;
        double  max_lat  = -1.0;
        for (auto& r : result.completed) {
            if (!r.is_parity && r.latency_ms > max_lat) {
                max_lat = r.latency_ms;
                loser   = r.shard_id;
            }
        }
        for (auto& r : result.completed) {
            if (r.is_parity) continue;
            double event = (r.shard_id == loser) ? 1.0 : 0.0;
            score_manager_.update_slowness(r.shard_id, stripe_hotness, event);
        }
    } else {
        // ── Phase B (proactive k+1 read) ─────────────────────────────────────
        // parity_win_event(i) = 1 iff parity is in `completed` AND
        //   either i is a straggler OR parity arrived faster than i.
        bool   parity_in_k  = false;
        double parity_lat   = std::numeric_limits<double>::max();
        for (auto& r : result.completed) {
            if (r.is_parity) {
                parity_in_k = true;
                parity_lat  = r.latency_ms;
                break;
            }
        }

        // Completed data shards.
        for (auto& r : result.completed) {
            if (r.is_parity) continue;
            double win = (parity_in_k && parity_lat < r.latency_ms) ? 1.0 : 0.0;
            score_manager_.update_slowness(r.shard_id, stripe_hotness, win);
            score_manager_.update_death(r.shard_id, stripe_hotness, win);
        }

        // Straggler data shards: parity definitely beat them if parity is in k.
        for (ShardId sid : result.stragglers) {
            double win = parity_in_k ? 1.0 : 0.0;
            score_manager_.update_slowness(sid, stripe_hotness, win);
            score_manager_.update_death(sid, stripe_hotness, win);
        }
    }
}

}  // namespace healthec::core
