#include "core/read_scheduler.h"
#include "core/score_manager.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace healthec::core;

// Always-active check (not disabled by NDEBUG).
[[noreturn]] static void fail(const char* expr, const char* file, int line) {
    std::fprintf(stderr, "FAIL  %s:%d  %s\n", file, line, expr);
    std::exit(1);
}
#define CHECK(expr) do { if (!(expr)) fail(#expr, __FILE__, __LINE__); } while (0)

static bool near(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) < tol;
}

// Helper: build a simple StripeLayout for k=2, m=1.
// data shards 0,1 on disks 0,1; parity shard 2 on disk 2.
static StripeLayout make_layout_2_1() {
    StripeLayout lay;
    lay.data_shards   = {0, 1};
    lay.parity_shards = {2};
    lay.disk_of       = {{0, 0}, {1, 1}, {2, 2}};
    return lay;
}

// Helper: instantaneous reader — returns immediately with the given latency_ms.
static ShardReader instant_reader(double latency_ms = 1.0) {
    return [latency_ms](DiskId, ShardId shard, bool is_parity) -> ShardReadResult {
        return {shard, is_parity, latency_ms};
    };
}

// Helper: push shard's S_i above theta_S via repeated loser events.
static void seed_slowness_above_threshold(ScoreManager& sm, ShardId shard) {
    for (int i = 0; i < 30; ++i)
        sm.update_slowness(shard, 1.0, 1.0);
    assert(sm.exceeds_slowness_threshold(shard));
}

// ── test 1: normal_k_reads ────────────────────────────────────────────────────
// No S_i above threshold → proactive_mode=false, completed has k entries,
// stragglers is empty.
static void test_normal_k_reads() {
    ScoreManager sm;
    ReadScheduler rs(2, 1, sm, instant_reader(1.0));

    auto lay = make_layout_2_1();
    auto res = rs.read_stripe(0, lay);
    rs.flush_score_updates();

    CHECK(!res.proactive_mode);
    CHECK(res.completed.size() == 2);
    CHECK(res.stragglers.empty());
    std::puts("PASS test_normal_k_reads");
}

// ── test 2: proactive_k1_reads ────────────────────────────────────────────────
// Any S_i > θ_S → proactive_mode=true, completed has k entries,
// stragglers has exactly 1 entry.
static void test_proactive_k1_reads() {
    ScoreManager sm;
    seed_slowness_above_threshold(sm, /*shard=*/0);

    ReadScheduler rs(2, 1, sm, instant_reader(1.0));
    auto lay = make_layout_2_1();
    auto res = rs.read_stripe(0, lay);
    rs.flush_score_updates();

    CHECK(res.proactive_mode);
    CHECK(res.completed.size() == 2);
    CHECK(res.stragglers.size() == 1);
    std::puts("PASS test_proactive_k1_reads");
}

// ── test 3: first_k_complete_drops_straggler ──────────────────────────────────
// Proactive mode + one very slow shard: that shard must end up in stragglers.
static void test_first_k_complete_drops_straggler() {
    ScoreManager sm;
    seed_slowness_above_threshold(sm, /*shard=*/0);

    // Shard 0 is slow (straggler candidate), shard 1 and parity (2) are fast.
    ShardReader slow_reader = [](DiskId, ShardId shard, bool is_parity) -> ShardReadResult {
        if (shard == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            return {shard, is_parity, 40.0};
        }
        return {shard, is_parity, 1.0};
    };

    ReadScheduler rs(2, 1, sm, slow_reader);
    auto lay = make_layout_2_1();
    auto res = rs.read_stripe(0, lay);
    rs.flush_score_updates();

    CHECK(res.proactive_mode);
    CHECK(res.completed.size() == 2);
    CHECK(res.stragglers.size() == 1);
    CHECK(res.stragglers[0] == 0);   // shard 0 is the straggler

    // Verify shard 0 is NOT in completed.
    for (auto& r : res.completed)
        CHECK(r.shard_id != 0);

    std::puts("PASS test_first_k_complete_drops_straggler");
}

// ── test 4: score_loser_updated ───────────────────────────────────────────────
// After a normal read, the data shard with the highest reported latency_ms
// gets loser_event=1 (S_i increases); others get event=0 (stay at 0).
static void test_score_loser_updated() {
    ScoreParams p;
    p.alpha_S = 0.2;
    ScoreManager sm(p);

    // Shard 0: lat=10 (faster), shard 1: lat=20 (slower → loser).
    ShardReader reader = [](DiskId, ShardId shard, bool is_parity) -> ShardReadResult {
        double lat = (shard == 1) ? 20.0 : 10.0;
        return {shard, is_parity, lat};
    };

    ReadScheduler rs(2, 1, sm, reader);
    auto lay = make_layout_2_1();
    rs.read_stripe(0, lay);
    rs.flush_score_updates();

    // Shard 1 was loser → event=1 → S_i = alpha_S * 1.0 > 0.
    CHECK(sm.get_slowness(1) > 0.0);
    // Shard 0 was winner → event=0 → S_i stays at 0.
    CHECK(near(sm.get_slowness(0), 0.0));

    std::puts("PASS test_score_loser_updated");
}

// ── test 5: parity_win_updates_death ─────────────────────────────────────────
// Proactive mode, parity arrives fastest, one data shard is straggler.
// → straggler data shard's D_i must increase (parity_win_event=1).
static void test_parity_win_updates_death() {
    ScoreParams p;
    p.alpha_S = 0.5;   // fast accumulation so we cross threshold quickly
    p.alpha_D = 0.1;
    ScoreManager sm(p);
    seed_slowness_above_threshold(sm, /*shard=*/0);

    // Register disk health so pick_best_parity works.
    sm.update_health(0, 0.0, 0.0, 0.0);
    sm.update_health(1, 0.0, 0.0, 0.0);
    sm.update_health(2, 0.0, 0.0, 0.0);

    // Shard 0: slow (straggler), shard 1: fast, parity (2): fast.
    ShardReader reader = [](DiskId, ShardId shard, bool is_parity) -> ShardReadResult {
        if (shard == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            return {shard, is_parity, 40.0};
        }
        return {shard, is_parity, 1.0};
    };

    ReadScheduler rs(2, 1, sm, reader);
    auto lay = make_layout_2_1();
    auto res = rs.read_stripe(0, lay);
    rs.flush_score_updates();

    // Parity (shard 2) must be in completed (it arrived fast).
    bool parity_in_completed = false;
    for (auto& r : res.completed)
        if (r.shard_id == 2) parity_in_completed = true;
    CHECK(parity_in_completed);

    // Shard 0 is straggler → parity won → D_i > 0.
    CHECK(res.stragglers.size() == 1 && res.stragglers[0] == 0);
    CHECK(sm.get_death(0) > 0.0);

    std::puts("PASS test_parity_win_updates_death");
}

// ── test 5b: parity_win_abs_blocks_straggler_close_race ─────────────────────
// A straggler's actual reported latency must still satisfy parity_win_abs_ms.
static void test_parity_win_abs_blocks_straggler_close_race() {
    ScoreParams p;
    p.alpha_S = 0.5;
    p.alpha_D = 0.1;
    p.parity_win_abs_ms = 15.0;
    ScoreManager sm(p);
    seed_slowness_above_threshold(sm, /*shard=*/0);

    sm.update_health(0, 0.0, 0.0, 0.0);
    sm.update_health(1, 0.0, 0.0, 0.0);
    sm.update_health(2, 0.0, 0.0, 0.0);

    ShardReader reader = [](DiskId, ShardId shard, bool is_parity) -> ShardReadResult {
        if (shard == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            return {shard, is_parity, 12.0};
        }
        return {shard, is_parity, is_parity ? 1.0 : 10.0};
    };

    ReadScheduler rs(2, 1, sm, reader);
    auto lay = make_layout_2_1();
    auto res = rs.read_stripe(0, lay);
    rs.flush_score_updates();

    CHECK(res.proactive_mode);
    CHECK(res.stragglers.size() == 1 && res.stragglers[0] == 0);
    CHECK(near(sm.get_death(0), 0.0));
    CHECK(near(sm.get_death(1), 0.0));

    std::puts("PASS test_parity_win_abs_blocks_straggler_close_race");
}

// ── test 5c: parity_win_abs_allows_straggler_large_win ───────────────────────
// The same straggler path must still count when parity wins by enough margin.
static void test_parity_win_abs_allows_straggler_large_win() {
    ScoreParams p;
    p.alpha_S = 0.5;
    p.alpha_D = 0.1;
    p.parity_win_abs_ms = 15.0;
    ScoreManager sm(p);
    seed_slowness_above_threshold(sm, /*shard=*/0);

    sm.update_health(0, 0.0, 0.0, 0.0);
    sm.update_health(1, 0.0, 0.0, 0.0);
    sm.update_health(2, 0.0, 0.0, 0.0);

    ShardReader reader = [](DiskId, ShardId shard, bool is_parity) -> ShardReadResult {
        if (shard == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            return {shard, is_parity, 40.0};
        }
        return {shard, is_parity, 1.0};
    };

    ReadScheduler rs(2, 1, sm, reader);
    auto lay = make_layout_2_1();
    auto res = rs.read_stripe(0, lay);
    rs.flush_score_updates();

    CHECK(res.proactive_mode);
    CHECK(res.stragglers.size() == 1 && res.stragglers[0] == 0);
    CHECK(sm.get_death(0) > 0.0);

    std::puts("PASS test_parity_win_abs_allows_straggler_large_win");
}

// ── test 6: parity_straggler_no_death ────────────────────────────────────────
// Proactive mode, parity is the straggler (all data shards finish first).
// → no parity_win_event → D_i must remain 0 for all data shards.
static void test_parity_straggler_no_death() {
    ScoreParams p;
    p.alpha_S = 0.5;
    ScoreManager sm(p);
    seed_slowness_above_threshold(sm, /*shard=*/0);

    // Parity (shard 2) is slow (straggler).
    ShardReader reader = [](DiskId, ShardId shard, bool is_parity) -> ShardReadResult {
        if (shard == 2) {   // parity
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            return {shard, is_parity, 40.0};
        }
        return {shard, is_parity, 1.0};
    };

    ReadScheduler rs(2, 1, sm, reader);
    auto lay = make_layout_2_1();
    auto res = rs.read_stripe(0, lay);
    rs.flush_score_updates();

    // Parity must be the straggler.
    CHECK(res.stragglers.size() == 1 && res.stragglers[0] == 2);
    // No parity_win_event → D_i stays 0 for both data shards.
    CHECK(near(sm.get_death(0), 0.0));
    CHECK(near(sm.get_death(1), 0.0));

    std::puts("PASS test_parity_straggler_no_death");
}

// ── test 7: flush_score_updates ───────────────────────────────────────────────
// Verify that flush_score_updates() waits for async score updates:
// after flushing, the loser's S_i is guaranteed to be updated.
static void test_flush_score_updates() {
    ScoreParams p;
    p.alpha_S = 0.2;
    ScoreManager sm(p);

    // Shard 1 is always the loser by latency_ms.
    ShardReader reader = [](DiskId, ShardId shard, bool is_parity) -> ShardReadResult {
        return {shard, is_parity, shard == 1 ? 50.0 : 5.0};
    };

    ReadScheduler rs(2, 1, sm, reader);
    auto lay = make_layout_2_1();
    rs.read_stripe(0, lay);

    // Without flush the update may still be running; call flush to synchronise.
    rs.flush_score_updates();

    // Now S_i(1) must reflect the loser event.
    CHECK(sm.get_slowness(1) > 0.0);
    CHECK(near(sm.get_slowness(0), 0.0));

    std::puts("PASS test_flush_score_updates");
}

// ── test concurrent read_stripe ───────────────────────────────────────────────
// 4 threads call read_stripe() concurrently on distinct stripe IDs.
// Verifies futures_mu_ protects pending_futures_ without data races or crashes.
static void test_concurrent_read_stripe() {
    ScoreManager sm;
    // Zero-latency reader so the test finishes quickly.
    ShardReader reader = [](DiskId, ShardId shard, bool is_parity) -> ShardReadResult {
        return {shard, is_parity, 0.0};
    };

    constexpr int K = 2, M = 1;
    ReadScheduler rs(K, M, sm, reader);

    auto make_layout = [](int base_shard, int base_disk) -> StripeLayout {
        StripeLayout lay;
        lay.data_shards   = {base_shard, base_shard + 1};
        lay.parity_shards = {base_shard + 2};
        lay.disk_of[base_shard]     = base_disk;
        lay.disk_of[base_shard + 1] = base_disk + 1;
        lay.disk_of[base_shard + 2] = base_disk + 2;
        return lay;
    };

    constexpr int NUM_THREADS = 4;
    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&rs, &make_layout, t]() {
            for (int i = 0; i < 20; ++i) {
                auto lay = make_layout(t * 100, t * 3);
                rs.read_stripe(t * 100 + i, lay);
            }
        });
    }
    for (auto& th : threads) th.join();
    rs.flush_score_updates();

    std::puts("PASS test_concurrent_read_stripe");
}

// ─────────────────────────────────────────────────────────────────────────────
int main() {
    test_normal_k_reads();
    test_proactive_k1_reads();
    test_first_k_complete_drops_straggler();
    test_score_loser_updated();
    test_parity_win_updates_death();
    test_parity_win_abs_blocks_straggler_close_race();
    test_parity_win_abs_allows_straggler_large_win();
    test_parity_straggler_no_death();
    test_flush_score_updates();
    test_concurrent_read_stripe();

    std::puts("All read_scheduler tests passed.");
    return 0;
}
