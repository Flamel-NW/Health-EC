// Integration test for T1.5: full pipeline verification.
//
// Pipeline under test:
//   WorkloadGenerator (Zipf) → ReadScheduler (proactive parity read)
//   → ScoreManager (S_i / D_i EMA) → MigrationScheduler::tick_once()
//   → DiskSimulator::migrate_shard() → ScoreManager::reset()
//
// Design principles:
//   - Deterministic: fixed seed, manual layout, tick_once() instead of background thread
//   - Fast: short latencies (1ms fast / 15ms slow), minimal stripe count
//   - Verifiable: explicit assertions on score values and shard file locations

#include "core/migration_scheduler.h"
#include "core/read_scheduler.h"
#include "core/score_manager.h"
#include "ec/jerasure_wrapper.h"
#include "sim/disk_simulator.h"
#include "sim/workload_generator.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using namespace healthec;

// ── Test configuration ─────────────────────────────────────────────────────────

static constexpr int    K           = 4;    // data shards
static constexpr int    M           = 2;    // parity shards
static constexpr int    NUM_DISKS   = 8;
static constexpr int    NUM_STRIPES = 5;
static constexpr int    SHARD_SIZE  = 512;  // bytes
static constexpr int    MAX_READS   = 1000; // loop safety bound

// ── Minimal test registry ──────────────────────────────────────────────────────

struct TestRegistry {
    std::mutex mu;
    std::unordered_map<core::StripeId, core::StripeLayout> layouts;
    std::unordered_map<core::ShardId,  core::DiskId>       shard_disk;

    core::StripeLayout copy_layout(core::StripeId sid) {
        std::lock_guard<std::mutex> lock(mu);
        return layouts.at(sid);
    }

    void update_shard_location(core::ShardId shard, core::DiskId dst) {
        std::lock_guard<std::mutex> lock(mu);
        shard_disk[shard] = dst;
        for (auto& [sid, layout] : layouts) {
            auto it = layout.disk_of.find(shard);
            if (it != layout.disk_of.end()) {
                it->second = dst;
                return;
            }
        }
    }

    core::DiskId current_disk(core::ShardId shard) {
        std::lock_guard<std::mutex> lock(mu);
        return shard_disk.at(shard);
    }
};

// ── Helpers ────────────────────────────────────────────────────────────────────

// ShardId = stripe * (K+M) + shard_index
static core::ShardId shard_id(int stripe, int idx) { return stripe * (K + M) + idx; }

// Build a deterministic layout:
//   Stripe 0 data shards → disks 2,3,4,5; parity → disks 6,7   (disk 2 = slow target)
//   Stripes 1-4          → round-robin starting from disk 0
static void build_layouts(TestRegistry& reg) {
    for (int s = 0; s < NUM_STRIPES; ++s) {
        core::StripeLayout layout;
        for (int i = 0; i < K + M; ++i) {
            core::ShardId sh;
            core::DiskId  dk;
            sh = shard_id(s, i);
            if (s == 0) {
                // Data shards on disks 2,3,4,5; parity on 6,7.
                dk = 2 + i;
            } else {
                // Round-robin across all disks.
                dk = (s * (K + M) + i) % NUM_DISKS;
            }
            layout.disk_of[sh] = dk;
            if (i < K) layout.data_shards.push_back(sh);
            else        layout.parity_shards.push_back(sh);
        }
        std::lock_guard<std::mutex> lock(reg.mu);
        reg.layouts[s] = layout;
        for (auto& [sh, dk] : layout.disk_of)
            reg.shard_disk[sh] = dk;
    }
}

// Write a stripe via JerasureCodec + DiskSimulator.
static void write_stripe(core::StripeId sid, const core::StripeLayout& layout,
                          sim::DiskSimulator& disk_sim, const ec::JerasureCodec& codec)
{
    std::vector<std::vector<char>> data(K, std::vector<char>(SHARD_SIZE));
    for (int i = 0; i < K; ++i)
        for (int b = 0; b < SHARD_SIZE; ++b)
            data[i][b] = static_cast<char>((sid * K + i + b) & 0xFF);

    auto parity = codec.encode(data);

    for (int i = 0; i < K; ++i)
        disk_sim.write_shard(layout.disk_of.at(layout.data_shards[i]),
                             layout.data_shards[i],
                             {data[i].begin(), data[i].end()});
    for (int j = 0; j < M; ++j)
        disk_sim.write_shard(layout.disk_of.at(layout.parity_shards[j]),
                             layout.parity_shards[j],
                             {parity[j].begin(), parity[j].end()});
}

// ── Test body ──────────────────────────────────────────────────────────────────

int main()
{
    const std::string test_dir =
        "/tmp/healthec_test_integration_" + std::to_string(getpid());
    fs::remove_all(test_dir);
    fs::create_directories(test_dir);

    bool all_passed = true;
    auto check = [&](bool cond, const char* msg) {
        if (!cond) { std::cerr << "FAIL: " << msg << "\n"; all_passed = false; }
        else         std::cout << "PASS: " << msg << "\n";
    };

    // ── Setup ─────────────────────────────────────────────────────────────────

    core::ScoreManager score_manager;

    // Register all disks with perfect health.
    for (int d = 0; d < NUM_DISKS; ++d)
        score_manager.update_health(d, 0.0, 0.0, 0.0);

    // Short latencies so the test completes quickly.
    sim::DiskProfile fast_profile;
    fast_profile.base_mean_ms   = 1.0;
    fast_profile.base_jitter_ms = 0.2;

    sim::DiskSimulator disk_sim(test_dir, NUM_DISKS, fast_profile);
    ec::JerasureCodec  codec(ec::EcParams{K, M});

    TestRegistry reg;
    build_layouts(reg);

    // Write all stripes.
    for (int s = 0; s < NUM_STRIPES; ++s)
        write_stripe(s, reg.copy_layout(s), disk_sim, codec);

    // Inject slow disk 2 with a modest latency that still allows fast completion.
    // slow_mean_ms=15 ensures shard0 (disk2) is reliably the straggler.
    {
        sim::DiskProfile slow_profile;
        slow_profile.slow_mode      = true;
        slow_profile.slow_mean_ms   = 15.0;
        slow_profile.slow_jitter_ms =  2.0;
        slow_profile.spike_prob     =  0.0;
        disk_sim.set_profile(2, slow_profile);
    }
    // Degrade H_d(disk2) so MigrationScheduler can find a healthier target.
    for (int i = 0; i < 30; ++i)
        score_manager.update_health(2, 0.95, 0.7, 0.1);
    double h2 = score_manager.get_health(2);
    check(h2 < 0.6, "H_d(disk 2) degraded below 0.6 after slow injection");

    // Wire ReadScheduler with DiskSimulator reader.
    core::ReadScheduler read_scheduler(K, M, score_manager, disk_sim.make_reader());

    // Wire MigrationScheduler with custom mover that also updates the registry.
    // Using tick_once() (no background thread) for determinism.
    core::ShardMover mover = [&](core::DiskId src, core::ShardId shard, core::DiskId dst) {
        disk_sim.migrate_shard(src, shard, dst);
        reg.update_shard_location(shard, dst);
    };
    core::MigrationScheduler migration_scheduler(score_manager, mover,
                                                  core::MigrationParams{4, 100.0});
    // No background thread: we call tick_once() manually.

    // WorkloadGenerator: Zipf s=1.0, seed=42
    // Stripe 0 gets ~43.8% of accesses.
    sim::WorkloadGenerator workload(NUM_STRIPES, 1.0, 42);

    // ── Read loop: run until D_i(shard0) > θ_D or MAX_READS ──────────────────

    const core::ShardId slow_shard = shard_id(0, 0);  // stripe 0, data shard 0, disk 2

    bool proactive_seen  = false;
    bool death_triggered = false;

    for (int r = 0; r < MAX_READS && !death_triggered; ++r) {
        core::StripeId sid    = workload.next_stripe();
        double         hot    = workload.hotness(sid);
        auto           layout = reg.copy_layout(sid);

        auto result = read_scheduler.read_stripe(sid, layout, hot);

        if (result.proactive_mode)
            proactive_seen = true;

        // Flush and check every 5 reads.
        if ((r + 1) % 5 == 0) {
            read_scheduler.flush_score_updates();
            if (score_manager.exceeds_death_threshold(slow_shard))
                death_triggered = true;
        }
    }

    read_scheduler.flush_score_updates();  // final flush

    // ── Assertion 1: proactive parity read triggered ──────────────────────────
    check(proactive_seen, "at least one proactive parity read triggered");

    // ── Assertion 2: D_i(slow_shard) exceeded migration threshold ────────────
    double di = score_manager.get_death(slow_shard);
    double si = score_manager.get_slowness(slow_shard);
    std::cout << "  D_i(shard " << slow_shard << ") = " << di << "\n";
    std::cout << "  S_i(shard " << slow_shard << ") = " << si << "\n";
    check(death_triggered, "D_i(slow_shard) exceeded theta_D within MAX_READS");

    if (!death_triggered) {
        // Cannot proceed with migration assertions.
        fs::remove_all(test_dir);
        return all_passed ? 0 : 1;
    }

    // ── Assertion 3: tick_once() migrates the shard and resets scores ─────────

    // Manually enqueue the shard (as main.cpp would do).
    core::DiskId old_disk = reg.current_disk(slow_shard);
    migration_scheduler.enqueue(slow_shard, old_disk, di);

    // Synchronous migration tick (no background thread).
    migration_scheduler.tick_once();

    // Check score reset.
    double di_after = score_manager.get_death(slow_shard);
    double si_after = score_manager.get_slowness(slow_shard);
    std::cout << "  After tick_once():\n";
    std::cout << "    D_i(shard " << slow_shard << ") = " << di_after << "\n";
    std::cout << "    S_i(shard " << slow_shard << ") = " << si_after << "\n";
    check(di_after == 0.0, "D_i reset to 0 after migration");
    check(si_after == 0.0, "S_i reset to 0 after migration");

    // Check shard was physically moved: new disk is no longer disk 2.
    core::DiskId new_disk = reg.current_disk(slow_shard);
    std::cout << "  Shard " << slow_shard << " migrated: disk "
              << old_disk << " → disk " << new_disk << "\n";
    check(new_disk != old_disk, "shard physically moved to a different disk");
    check(new_disk != 2,        "shard no longer on the slow disk (disk 2)");

    // Verify shard file accessible on new disk.
    bool readable_on_new = false;
    try {
        disk_sim.read_data(new_disk, slow_shard);
        readable_on_new = true;
    } catch (...) {}
    check(readable_on_new, "shard file readable on new disk after migration");

    // Verify shard file no longer on old disk.
    bool readable_on_old = false;
    try {
        disk_sim.read_data(old_disk, slow_shard);
        readable_on_old = true;
    } catch (...) {}
    check(!readable_on_old, "shard file removed from old disk after migration");

    // ── Cleanup ───────────────────────────────────────────────────────────────
    fs::remove_all(test_dir);

    std::cout << "\n" << (all_passed ? "ALL TESTS PASSED" : "SOME TESTS FAILED") << "\n";
    return all_passed ? 0 : 1;
}
