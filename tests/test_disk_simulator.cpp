#include "sim/disk_simulator.h"
#include "core/read_scheduler.h"
#include "core/score_manager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <numeric>
#include <string>

namespace fs = std::filesystem;
using namespace healthec::sim;
using namespace healthec::core;

// Always-active check (not disabled by NDEBUG).
[[noreturn]] static void fail(const char* expr, const char* file, int line) {
    std::fprintf(stderr, "FAIL  %s:%d  %s\n", file, line, expr);
    std::exit(1);
}
#define CHECK(expr) do { if (!(expr)) fail(#expr, __FILE__, __LINE__); } while (0)

// Root tmp directory for this test run; cleaned up in main().
static const std::string TMP_ROOT = "/tmp/healthec_ds_test";

static std::string test_dir(const char* name) {
    return TMP_ROOT + "/" + name;
}

// ── test 1: write_read_roundtrip ──────────────────────────────────────────────
// write_shard() persists bytes; read_data() returns identical bytes.
static void test_write_read_roundtrip() {
    DiskSimulator ds(test_dir("roundtrip"), 2);

    std::vector<uint8_t> payload = {0x48, 0x65, 0x61, 0x6C, 0x74, 0x68,
                                    0xEC, 0x00, 0xFF, 0xAB};
    ds.write_shard(/*disk=*/0, /*shard=*/10, payload);

    auto got = ds.read_data(0, 10);
    CHECK(got.size() == payload.size());
    CHECK(got == payload);

    // Overwrite with different data; verify update is visible.
    std::vector<uint8_t> v2 = {0x01, 0x02, 0x03};
    ds.write_shard(0, 10, v2);
    CHECK(ds.read_data(0, 10) == v2);

    std::puts("PASS test_write_read_roundtrip");
}

// ── test 2: shard_path_layout ─────────────────────────────────────────────────
// Virtual disk directories and shard files follow the naming convention.
static void test_shard_path_layout() {
    std::string base = test_dir("layout");
    DiskSimulator ds(base, 3);

    ds.write_shard(/*disk=*/0, /*shard=*/5,  {0xAA});
    ds.write_shard(/*disk=*/2, /*shard=*/99, {0xBB});

    // Check directory structure.
    CHECK(fs::is_directory(base + "/disk0"));
    CHECK(fs::is_directory(base + "/disk2"));

    // Check file names match convention: shard{i}.bin
    CHECK(fs::exists(base + "/disk0/shard5.bin"));
    CHECK(fs::exists(base + "/disk2/shard99.bin"));

    // A non-written shard must NOT exist.
    CHECK(!fs::exists(base + "/disk1/shard5.bin"));

    std::puts("PASS test_shard_path_layout");
}

// ── test 3: latency_normal_range ─────────────────────────────────────────────
// Under the normal latency profile, sampled latencies are non-negative and
// the sample mean is within ±4σ of the configured mean (statistical guard).
static void test_latency_normal_range() {
    DiskProfile p;
    p.base_mean_ms   = 2.0;   // keep sleep short: 2ms × 20 = ~40ms
    p.base_jitter_ms = 0.4;
    p.slow_mode      = false;

    DiskSimulator ds(test_dir("latency_normal"), 1, p);

    constexpr int N = 20;
    double sum = 0.0;
    for (int i = 0; i < N; ++i) {
        auto r = ds.read_shard(/*disk=*/0, /*shard=*/i, false);
        CHECK(r.latency_ms >= 0.0);   // non-negative
        sum += r.latency_ms;
    }
    double mean = sum / N;
    // Mean must be within ±4σ (extremely conservative; fails only on hardware issues).
    double margin = 4.0 * p.base_jitter_ms;
    CHECK(mean >= p.base_mean_ms - margin);
    CHECK(mean <= p.base_mean_ms + margin);

    std::puts("PASS test_latency_normal_range");
}

// ── test 4: latency_slow_elevated ────────────────────────────────────────────
// Slow mode must produce latencies with a higher average than normal mode.
static void test_latency_slow_elevated() {
    DiskProfile normal_p;
    normal_p.base_mean_ms   = 1.0;
    normal_p.base_jitter_ms = 0.2;

    DiskProfile slow_p = normal_p;
    slow_p.slow_mode      = true;
    slow_p.slow_mean_ms   = 8.0;   // distinctly higher
    slow_p.slow_jitter_ms = 1.0;

    constexpr int N = 10;

    // Normal-mode measurements.
    DiskSimulator ds_normal(test_dir("slow_normal"), 1, normal_p);
    double sum_normal = 0.0;
    for (int i = 0; i < N; ++i)
        sum_normal += ds_normal.read_shard(0, i, false).latency_ms;

    // Slow-mode measurements.
    DiskSimulator ds_slow(test_dir("slow_slow"), 1, slow_p);
    double sum_slow = 0.0;
    for (int i = 0; i < N; ++i)
        sum_slow += ds_slow.read_shard(0, i, false).latency_ms;

    CHECK(sum_slow / N > sum_normal / N);

    std::puts("PASS test_latency_slow_elevated");
}

// ── test 5: runtime_profile_change ───────────────────────────────────────────
// set_profile() and set_slow() must take effect for subsequent reads.
static void test_runtime_profile_change() {
    DiskProfile fast;
    fast.base_mean_ms   = 1.0;
    fast.base_jitter_ms = 0.1;

    DiskSimulator ds(test_dir("runtime_change"), 1, fast);

    // Measure average under fast profile.
    constexpr int N = 8;
    double sum_fast = 0.0;
    for (int i = 0; i < N; ++i)
        sum_fast += ds.read_shard(0, i, false).latency_ms;
    double mean_fast = sum_fast / N;

    // Switch to slow profile at runtime.
    DiskProfile slow;
    slow.base_mean_ms   = 1.0;
    slow.base_jitter_ms = 0.1;
    slow.slow_mode      = true;
    slow.slow_mean_ms   = 10.0;
    slow.slow_jitter_ms = 1.0;
    ds.set_profile(0, slow);

    double sum_slow = 0.0;
    for (int i = N; i < 2 * N; ++i)
        sum_slow += ds.read_shard(0, i, false).latency_ms;
    double mean_slow = sum_slow / N;

    CHECK(mean_slow > mean_fast);

    // Use set_slow() convenience toggle: turn slow off.
    ds.set_slow(0, false);
    DiskProfile current = ds.get_profile(0);
    CHECK(!current.slow_mode);

    std::puts("PASS test_runtime_profile_change");
}

// ── test 6: make_reader_integration ──────────────────────────────────────────
// make_reader() returns a ShardReader usable by ReadScheduler.
// After read_stripe(), all completed shards have latency_ms > 0.
static void test_make_reader_integration() {
    DiskProfile p;
    p.base_mean_ms   = 1.0;
    p.base_jitter_ms = 0.1;

    DiskSimulator ds(test_dir("reader_integration"), 3, p);

    // Pre-write shards so read_shard() can perform real file I/O.
    ds.write_shard(0, /*shard=*/0, {0x00, 0x01});
    ds.write_shard(1, /*shard=*/1, {0x02, 0x03});
    ds.write_shard(2, /*shard=*/2, {0x04, 0x05});   // parity

    ScoreManager sm;
    ReadScheduler rs(/*k=*/2, /*m=*/1, sm, ds.make_reader());

    StripeLayout layout;
    layout.data_shards   = {0, 1};
    layout.parity_shards = {2};
    layout.disk_of       = {{0, 0}, {1, 1}, {2, 2}};

    auto result = rs.read_stripe(/*sid=*/0, layout);
    rs.flush_score_updates();

    CHECK(!result.proactive_mode);                    // no S_i above threshold
    CHECK(result.completed.size() == 2);
    for (auto& r : result.completed)
        CHECK(r.latency_ms > 0.0);   // injected delay was positive

    std::puts("PASS test_make_reader_integration");
}

// ── test 7: invalid_profile_rejected ─────────────────────────────────────────
static void test_invalid_profile_rejected() {
    bool threw = false;
    try {
        DiskProfile p;
        p.base_jitter_ms = -1.0;
        DiskSimulator ds(test_dir("bad_ctor"), 1, p);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    DiskSimulator ds(test_dir("bad_set"), 1);
    DiskProfile bad;
    bad.spike_prob = 1.5;
    threw = false;
    try {
        ds.set_profile(0, bad);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    bad = DiskProfile{};
    bad.base_mean_ms = std::numeric_limits<double>::infinity();
    threw = false;
    try {
        ds.set_profile(0, bad);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    std::puts("PASS test_invalid_profile_rejected");
}

// ── test 8: zero_jitter_is_deterministic ─────────────────────────────────────
static void test_zero_jitter_is_deterministic() {
    DiskProfile p;
    p.base_mean_ms = 3.0;
    p.base_jitter_ms = 0.0;
    DiskSimulator ds(test_dir("zero_jitter"), 1, p);

    CHECK(std::fabs(ds.sample_latency_ms(0) - 3.0) < 1e-9);
    CHECK(std::fabs(ds.sample_latency_ms(0) - 3.0) < 1e-9);

    std::puts("PASS test_zero_jitter_is_deterministic");
}

// ─────────────────────────────────────────────────────────────────────────────
int main() {
    // Remove any leftover state from a previous run.
    fs::remove_all(TMP_ROOT);

    test_write_read_roundtrip();
    test_shard_path_layout();
    test_latency_normal_range();
    test_latency_slow_elevated();
    test_runtime_profile_change();
    test_make_reader_integration();
    test_invalid_profile_rejected();
    test_zero_jitter_is_deterministic();

    // Clean up temporary files.
    fs::remove_all(TMP_ROOT);

    std::puts("All disk_simulator tests passed.");
    return 0;
}
