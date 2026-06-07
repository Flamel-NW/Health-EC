#include "core/migration_scheduler.h"
#include "core/score_manager.h"
#include "sim/disk_simulator.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace healthec::core;
using namespace healthec::sim;

[[noreturn]] static void fail(const char* expr, const char* file, int line) {
    std::fprintf(stderr, "FAIL  %s:%d  %s\n", file, line, expr);
    std::exit(1);
}
#define CHECK(expr) do { if (!(expr)) fail(#expr, __FILE__, __LINE__); } while (0)

static const std::string TMP_ROOT = "/tmp/healthec_ms_test";
static std::string test_dir(const char* name) { return TMP_ROOT + "/" + name; }

// ── test 1: basic migration ───────────────────────────────────────────────────
// disk0 low H_d, disk1 high H_d → shard migrates to disk1.
static void test_migrate_basic() {
    const std::string base = test_dir("basic");
    DiskSimulator ds(base, 2);
    ds.write_shard(0, 42, {0xDE, 0xAD, 0xBE, 0xEF});

    ScoreManager sm;
    // disk0 unhealthy (high latency), disk1 healthy.
    sm.update_health(0, /*l*/0.9, /*q*/0.9, /*e*/0.0);
    sm.update_health(1, /*l*/0.1, /*q*/0.1, /*e*/0.0);

    MigrationParams p; p.budget_B = 1;
    MigrationScheduler ms_sched(sm, ds.make_mover(), p);
    ms_sched.enqueue(/*shard*/42, /*disk*/0, /*death_score*/0.9);
    ms_sched.tick_once();

    CHECK( fs::exists(base + "/disk1/shard42.bin"));
    CHECK(!fs::exists(base + "/disk0/shard42.bin"));
    // Verify data integrity after migration.
    auto data = ds.read_data(1, 42);
    CHECK(data == (std::vector<uint8_t>{0xDE, 0xAD, 0xBE, 0xEF}));
    std::puts("PASS test_migrate_basic");
}

// ── test 2: budget enforcement ────────────────────────────────────────────────
// 5 shards queued, budget_B=2 → only top-2 (by death_score) migrate per tick.
static void test_budget_limit() {
    const std::string base = test_dir("budget");
    DiskSimulator ds(base, 2);
    for (int i = 0; i < 5; ++i)
        ds.write_shard(0, i, {static_cast<uint8_t>(i)});

    ScoreManager sm;
    sm.update_health(0, 0.9, 0.0, 0.0);  // low
    sm.update_health(1, 0.1, 0.0, 0.0);  // high

    MigrationParams p; p.budget_B = 2;
    MigrationScheduler ms_sched(sm, ds.make_mover(), p);

    // Enqueue with varying death scores; shards 3 and 4 have highest scores.
    ms_sched.enqueue(0, 0, 0.50);
    ms_sched.enqueue(1, 0, 0.60);
    ms_sched.enqueue(2, 0, 0.55);
    ms_sched.enqueue(3, 0, 0.95);  // top-1
    ms_sched.enqueue(4, 0, 0.90);  // top-2
    ms_sched.tick_once();

    // Shards 3 and 4 should be on disk1; shards 0-2 stay on disk0.
    CHECK( fs::exists(base + "/disk1/shard3.bin"));
    CHECK( fs::exists(base + "/disk1/shard4.bin"));
    CHECK(!fs::exists(base + "/disk1/shard0.bin"));
    CHECK(!fs::exists(base + "/disk1/shard1.bin"));
    CHECK(!fs::exists(base + "/disk1/shard2.bin"));
    std::puts("PASS test_budget_limit");
}

// ── test 3: no valid target ───────────────────────────────────────────────────
// All disks have the same H_d → no migration, shard stays in queue.
static void test_no_valid_target() {
    const std::string base = test_dir("no_target");
    DiskSimulator ds(base, 2);
    ds.write_shard(0, 7, {0xFF});

    ScoreManager sm;
    // Same health → no disk strictly better than current.
    sm.update_health(0, 0.5, 0.5, 0.0);
    sm.update_health(1, 0.5, 0.5, 0.0);

    MigrationParams p; p.budget_B = 4;
    MigrationScheduler ms_sched(sm, ds.make_mover(), p);
    ms_sched.enqueue(7, 0, 0.8);
    ms_sched.tick_once();

    // File must remain on disk0.
    CHECK( fs::exists(base + "/disk0/shard7.bin"));
    CHECK(!fs::exists(base + "/disk1/shard7.bin"));
    std::puts("PASS test_no_valid_target");
}

// ── test 4: score reset after migration ───────────────────────────────────────
// After migrating, ScoreManager::reset() must zero S_i and D_i.
static void test_score_reset() {
    const std::string base = test_dir("score_reset");
    DiskSimulator ds(base, 2);
    ds.write_shard(0, 99, {0xAB});

    ScoreManager sm;
    sm.update_health(0, 0.9, 0.0, 0.0);
    sm.update_health(1, 0.1, 0.0, 0.0);

    // Prime non-zero S_i and D_i.
    sm.update_slowness(99, 1.0, 1.0);
    sm.update_death(99, 1.0, 1.0);
    CHECK(sm.get_slowness(99) > 0.0);
    CHECK(sm.get_death(99)    > 0.0);

    MigrationParams p; p.budget_B = 1;
    MigrationScheduler ms_sched(sm, ds.make_mover(), p);
    ms_sched.enqueue(99, 0, sm.get_death(99));
    ms_sched.tick_once();

    CHECK(sm.get_slowness(99) == 0.0);
    CHECK(sm.get_death(99)    == 0.0);
    std::puts("PASS test_score_reset");
}

// ── test 5: enqueue upsert ────────────────────────────────────────────────────
// Re-enqueueing a shard with a higher death_score updates its priority.
static void test_enqueue_upsert() {
    const std::string base = test_dir("upsert");
    DiskSimulator ds(base, 2);
    ds.write_shard(0, 10, {0x01});
    ds.write_shard(0, 20, {0x02});

    ScoreManager sm;
    sm.update_health(0, 0.9, 0.0, 0.0);
    sm.update_health(1, 0.1, 0.0, 0.0);

    MigrationParams p; p.budget_B = 1;  // only 1 migrated per tick
    MigrationScheduler ms_sched(sm, ds.make_mover(), p);

    ms_sched.enqueue(10, 0, 0.95);  // shard 10 initially highest
    ms_sched.enqueue(20, 0, 0.50);
    // Upsert: bump shard 20 above shard 10.
    ms_sched.enqueue(20, 0, 0.99);
    ms_sched.tick_once();

    // Shard 20 (higher score after upsert) should have migrated.
    CHECK( fs::exists(base + "/disk1/shard20.bin"));
    CHECK(!fs::exists(base + "/disk1/shard10.bin"));
    std::puts("PASS test_enqueue_upsert");
}

// ── test 6: background thread lifecycle ──────────────────────────────────────
// start() → enqueue → background thread migrates → stop() joins cleanly.
static void test_background_thread() {
    const std::string base = test_dir("bg_thread");
    DiskSimulator ds(base, 2);
    ds.write_shard(0, 55, {0xCC, 0xDD});

    ScoreManager sm;
    sm.update_health(0, 0.9, 0.0, 0.0);
    sm.update_health(1, 0.1, 0.0, 0.0);

    MigrationParams p;
    p.budget_B = 1;
    p.tick_ms  = 20.0;  // fast tick for testing

    MigrationScheduler ms_sched(sm, ds.make_mover(), p);
    ms_sched.start();
    ms_sched.enqueue(55, 0, 0.85);

    // Wait up to 500 ms for the background thread to complete migration.
    constexpr int max_wait_ms = 500;
    constexpr int poll_ms     = 10;
    for (int waited = 0; waited < max_wait_ms; waited += poll_ms) {
        if (fs::exists(base + "/disk1/shard55.bin")) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
    }

    ms_sched.stop();  // must not hang

    CHECK( fs::exists(base + "/disk1/shard55.bin"));
    CHECK(!fs::exists(base + "/disk0/shard55.bin"));
    std::puts("PASS test_background_thread");
}

// ── main ─────────────────────────────────────────────────────────────────────

int main() {
    fs::remove_all(TMP_ROOT);
    test_migrate_basic();
    test_budget_limit();
    test_no_valid_target();
    test_score_reset();
    test_enqueue_upsert();
    test_background_thread();
    fs::remove_all(TMP_ROOT);
    std::puts("All migration_scheduler tests passed.");
    return 0;
}
