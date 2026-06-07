#include "core/score_manager.h"
#include "core/placement_policy.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

using namespace healthec::core;

// Always-active check (unlike assert, not disabled by NDEBUG).
[[noreturn]] static void fail(const char* expr, const char* file, int line) {
    std::fprintf(stderr, "FAIL  %s:%d  %s\n", file, line, expr);
    std::exit(1);
}
#define CHECK(expr) do { if (!(expr)) fail(#expr, __FILE__, __LINE__); } while(0)

// Floating-point comparison with absolute tolerance.
static bool near(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) < tol;
}

// ── test_health_default ───────────────────────────────────────────────────────
// An unregistered disk must have H_d = 1.0 (perfectly healthy default).
static void test_health_default() {
    ScoreManager sm;
    CHECK(near(sm.get_health(42), 1.0));
    CHECK(near(sm.get_health(0),  1.0));
    std::puts("PASS test_health_default");
}

// ── test_health_ema_update ────────────────────────────────────────────────────
// Inject a maximally degraded signal (l=1, q=1, e=1 → f=0) repeatedly and
// verify that H_d converges toward 0 via the EMA recurrence.
static void test_health_ema_update() {
    ScoreParams p;
    p.alpha_H = 0.1;
    ScoreManager sm(p);

    double expected = 1.0;
    for (int i = 0; i < 50; ++i) {
        sm.update_health(0, 1.0, 1.0, 1.0);   // f = 0
        expected = (1.0 - p.alpha_H) * expected;
    }
    CHECK(near(sm.get_health(0), expected, 1e-12));
    // After 50 steps with alpha=0.1 the score is (0.9)^50 ≈ 0.00515
    CHECK(sm.get_health(0) < 0.01);
    std::puts("PASS test_health_ema_update");
}

// ── test_health_sorted ────────────────────────────────────────────────────────
// Three disks with distinct health signals; verify descending sort.
static void test_health_sorted() {
    ScoreManager sm;
    // Disk 0: best — no degradation (f = 1.0)
    sm.update_health(0, 0.0, 0.0, 0.0);
    // Disk 1: moderate — partial degradation
    sm.update_health(1, 0.5, 0.5, 0.5);
    // Disk 2: worst — full degradation (f = 0.0)
    sm.update_health(2, 1.0, 1.0, 1.0);

    auto sorted = sm.get_all_disks_sorted_by_health();
    CHECK(sorted.size() == 3);
    CHECK(sm.get_health(sorted[0]) >= sm.get_health(sorted[1]));
    CHECK(sm.get_health(sorted[1]) >= sm.get_health(sorted[2]));
    CHECK(sorted[0] == 0);
    CHECK(sorted[2] == 2);
    std::puts("PASS test_health_sorted");
}

// ── test_placement_select ─────────────────────────────────────────────────────
// PlacementPolicy::select_disks(2,1) must return the top-3 disks by H_d.
static void test_placement_select() {
    ScoreManager sm;
    sm.update_health(0, 0.0, 0.0, 0.0);   // healthiest
    sm.update_health(1, 0.3, 0.1, 0.0);
    sm.update_health(2, 0.7, 0.5, 0.1);
    sm.update_health(3, 1.0, 1.0, 1.0);   // worst

    PlacementPolicy pp(sm);
    auto selected = pp.select_disks(2, 1);   // need 3 disks
    CHECK(selected.size() == 3);

    // Top-3 must not include disk 3 (worst).
    for (DiskId id : selected)
        CHECK(id != 3);

    // Verify exception when not enough disks.
    bool threw = false;
    try { pp.select_disks(3, 2); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);

    std::puts("PASS test_placement_select");
}

// ── test_slowness_accumulate ──────────────────────────────────────────────────
// Repeated loser events (event=1, w_s=1) must monotonically increase S_i.
static void test_slowness_accumulate() {
    ScoreManager sm;
    double prev = 0.0;
    for (int i = 0; i < 20; ++i) {
        sm.update_slowness(7, 1.0, 1.0);
        double cur = sm.get_slowness(7);
        CHECK(cur > prev);
        prev = cur;
    }
    std::puts("PASS test_slowness_accumulate");
}

// ── test_slowness_decay ───────────────────────────────────────────────────────
// After seeding S_i to a high value, event=0 updates must decay it.
static void test_slowness_decay() {
    ScoreParams p;
    p.alpha_S = 0.2;
    ScoreManager sm(p);

    // Push S_i up.
    for (int i = 0; i < 30; ++i)
        sm.update_slowness(5, 1.0, 1.0);
    double high = sm.get_slowness(5);
    CHECK(high > 0.5);

    // Let it decay with event=0.
    for (int i = 0; i < 30; ++i)
        sm.update_slowness(5, 1.0, 0.0);
    double low = sm.get_slowness(5);
    // After 30 decay steps with alpha=0.2: factor = (0.8)^30 ≈ 0.00124
    CHECK(low < high * 0.01);
    std::puts("PASS test_slowness_decay");
}

// ── test_slowness_threshold ───────────────────────────────────────────────────
// exceeds_slowness_threshold flips at theta_S; check both sides.
static void test_slowness_threshold() {
    ScoreParams p;
    p.alpha_S = 0.5;   // fast accumulation for test speed
    p.theta_S = 0.8;
    ScoreManager sm(p);

    // Initially below threshold.
    CHECK(!sm.exceeds_slowness_threshold(1));

    // Drive S_i well above theta_S.
    for (int i = 0; i < 20; ++i)
        sm.update_slowness(1, 1.0, 1.0);
    CHECK(sm.get_slowness(1) > p.theta_S);
    CHECK(sm.exceeds_slowness_threshold(1));

    // Decay back below threshold.
    for (int i = 0; i < 30; ++i)
        sm.update_slowness(1, 1.0, 0.0);
    CHECK(sm.get_slowness(1) < p.theta_S);
    CHECK(!sm.exceeds_slowness_threshold(1));

    std::puts("PASS test_slowness_threshold");
}

// ── test_death_slower_decay ───────────────────────────────────────────────────
// Under identical seeding and decay steps, D_i must remain relatively higher
// than S_i because alpha_D < alpha_S (slower EMA → slower decay).
static void test_death_slower_decay() {
    ScoreParams p;
    p.alpha_S = 0.2;
    p.alpha_D = 0.05;
    ScoreManager sm(p);

    // Seed both scores with the same number of win events.
    for (int i = 0; i < 40; ++i) {
        sm.update_slowness(9, 1.0, 1.0);
        sm.update_death(9, 1.0, 1.0);
    }
    double s_high = sm.get_slowness(9);
    double d_high = sm.get_death(9);

    // Decay both with zero events.
    for (int i = 0; i < 20; ++i) {
        sm.update_slowness(9, 1.0, 0.0);
        sm.update_death(9, 1.0, 0.0);
    }
    double s_low = sm.get_slowness(9);
    double d_low = sm.get_death(9);

    // Both must have decayed.
    CHECK(s_low < s_high);
    CHECK(d_low < d_high);
    // D_i decays slower: relative retention d_low/d_high > s_low/s_high.
    CHECK(d_low / d_high > s_low / s_high);

    std::puts("PASS test_death_slower_decay");
}

// ── test_reset ────────────────────────────────────────────────────────────────
// After reset(), both S_i and D_i must read as 0.
static void test_reset() {
    ScoreManager sm;
    for (int i = 0; i < 20; ++i) {
        sm.update_slowness(3, 1.0, 1.0);
        sm.update_death(3, 1.0, 1.0);
    }
    CHECK(sm.get_slowness(3) > 0.0);
    CHECK(sm.get_death(3)    > 0.0);

    sm.reset(3);
    CHECK(near(sm.get_slowness(3), 0.0));
    CHECK(near(sm.get_death(3),    0.0));
    std::puts("PASS test_reset");
}

// ── test_placement_insufficient_disks ────────────────────────────────────────
// select_disks must throw when fewer disks are registered than needed.
static void test_placement_insufficient_disks() {
    ScoreManager sm;
    PlacementPolicy pp(sm);
    bool threw = false;
    try { pp.select_disks(4, 2); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    std::puts("PASS test_placement_insufficient_disks");
}

int main() {
    test_health_default();
    test_health_ema_update();
    test_health_sorted();
    test_placement_select();
    test_slowness_accumulate();
    test_slowness_decay();
    test_slowness_threshold();
    test_death_slower_decay();
    test_reset();
    test_placement_insufficient_disks();

    std::puts("All score_manager tests passed.");
    return 0;
}
