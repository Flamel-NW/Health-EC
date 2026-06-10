// Unit tests for compute_loser_significant() and parity_won() pure functions.
//
// Coverage:
//   1. Default sig params (0, 0): all-equal latency → argmax returned (guards instant_reader)
//   2. Default sig params (0, 0): distinct latencies → max-latency shard returned
//   3. sig_ratio=0.5: loser NOT 1.5× median → nullopt
//   4. sig_ratio=0.5: loser exactly 1.5× median (>= boundary) → significant
//   5. sig_abs_ms=15: loser below median+15 → nullopt
//   6. sig_abs_ms=15: loser exactly median+15 (>= boundary) → significant
//   7. Both conditions (AND): ratio fails → nullopt even if abs passes
//   8. Both conditions (AND): both pass → significant
//   9. Even-k median linear interpolation (k=4)
//  10. Single shard → always significant (no comparison possible)
//  11. parity_won: parity faster → true; parity slower → false; equal → false

#include "core/read_scheduler.h"

#include <cstdio>
#include <cstdlib>

using namespace healthec::core;

[[noreturn]] static void fail(const char* expr, const char* file, int line) {
    std::fprintf(stderr, "FAIL  %s:%d  %s\n", file, line, expr);
    std::exit(1);
}
#define CHECK(expr) do { if (!(expr)) fail(#expr, __FILE__, __LINE__); } while (0)

// Shorthand: build data_lat vector from initializer list.
static std::vector<std::pair<ShardId, double>> dl(
    std::initializer_list<std::pair<ShardId, double>> init) {
    return init;
}

// ── test 1: default 0,0 all-equal latency → argmax (first shard) ─────────────
static void test_default_equal_latency() {
    auto res = compute_loser_significant(dl({{0, 1.0}, {1, 1.0}}), 0.0, 0.0);
    CHECK(res.has_value());
    // argmax with first-wins tie-breaking → shard 0
    CHECK(*res == 0);
    std::puts("PASS test_default_equal_latency");
}

// ── test 2: default 0,0 distinct latencies → max shard ───────────────────────
static void test_default_distinct_latency() {
    auto res = compute_loser_significant(dl({{0, 10.0}, {1, 20.0}}), 0.0, 0.0);
    CHECK(res.has_value());
    CHECK(*res == 1);
    std::puts("PASS test_default_distinct_latency");
}

// ── test 3: sig_ratio=0.5, loser < 1.5× median → nullopt ─────────────────────
static void test_ratio_below_threshold() {
    // data: [10, 20], median=15, loser=20, need 20 >= 15*1.5=22.5 → NO
    auto res = compute_loser_significant(dl({{0, 10.0}, {1, 20.0}}), 0.5, 0.0);
    CHECK(!res.has_value());
    std::puts("PASS test_ratio_below_threshold");
}

// ── test 4: sig_ratio=0.5, loser exactly 1.5× median → significant (>=) ──────
static void test_ratio_at_boundary() {
    // data: [10, 30], median=20, loser=30, 30 >= 20*1.5=30 → YES (>=)
    auto res = compute_loser_significant(dl({{0, 10.0}, {1, 30.0}}), 0.5, 0.0);
    CHECK(res.has_value());
    CHECK(*res == 1);
    std::puts("PASS test_ratio_at_boundary");
}

// ── test 5: sig_abs_ms=15, loser below median+15 → nullopt ───────────────────
static void test_abs_below_threshold() {
    // data: [10, 20], median=15, loser=20, need 20 >= 15+15=30 → NO
    auto res = compute_loser_significant(dl({{0, 10.0}, {1, 20.0}}), 0.0, 15.0);
    CHECK(!res.has_value());
    std::puts("PASS test_abs_below_threshold");
}

// ── test 6: sig_abs_ms=15, loser exactly median+15 → significant (>=) ─────────
static void test_abs_at_boundary() {
    // data: [10, 40], median=25, loser=40, 40 >= 25+15=40 → YES (>=)
    auto res = compute_loser_significant(dl({{0, 10.0}, {1, 40.0}}), 0.0, 15.0);
    CHECK(res.has_value());
    CHECK(*res == 1);
    std::puts("PASS test_abs_at_boundary");
}

// ── test 7: AND — ratio fails, abs passes → nullopt ───────────────────────────
static void test_and_ratio_fails() {
    // data: [10, 25], median=17.5, loser=25
    // ratio: 25 >= 17.5*1.5=26.25 → NO (fails)
    // abs  : 25 >= 17.5+5=22.5 → YES (passes)
    // AND → nullopt
    auto res = compute_loser_significant(dl({{0, 10.0}, {1, 25.0}}), 0.5, 5.0);
    CHECK(!res.has_value());
    std::puts("PASS test_and_ratio_fails");
}

// ── test 8: AND — both conditions pass → significant ──────────────────────────
static void test_and_both_pass() {
    // data: [10, 40], median=25, loser=40
    // ratio: 40 >= 25*1.5=37.5 → YES
    // abs  : 40 >= 25+5=30 → YES
    auto res = compute_loser_significant(dl({{0, 10.0}, {1, 40.0}}), 0.5, 5.0);
    CHECK(res.has_value());
    CHECK(*res == 1);
    std::puts("PASS test_and_both_pass");
}

// ── test 9: even-k=4 median linear interpolation ─────────────────────────────
static void test_even_k_median_interpolation() {
    // data: [5,10,15,50], sorted median: idx=1.5 → (10+15)/2=12.5
    // loser=50, ratio=0.5: 50 >= 12.5*1.5=18.75 → YES
    auto res = compute_loser_significant(
        dl({{0, 5.0}, {1, 10.0}, {2, 15.0}, {3, 50.0}}), 0.5, 0.0);
    CHECK(res.has_value());
    CHECK(*res == 3);
    std::puts("PASS test_even_k_median_interpolation");
}

// ── test 10: single shard with default params → always significant ────────────
static void test_single_shard() {
    // k=1: sorted=[7], median=7, loser=7, defaults 0,0 → 7>=7*1.0 and 7>=7+0 → YES
    auto res = compute_loser_significant(dl({{5, 7.0}}), 0.0, 0.0);
    CHECK(res.has_value());
    CHECK(*res == 5);
    std::puts("PASS test_single_shard");
}

// ── test 11: parity_won ───────────────────────────────────────────────────────
static void test_parity_won() {
    // Default abs_ms=0: any strict win counts.
    CHECK( parity_won(5.0, 10.0));        // parity faster → true
    CHECK(!parity_won(10.0,  5.0));       // parity slower → false
    CHECK(!parity_won(10.0, 10.0));       // equal → false (not strictly less)

    // abs_ms > 0: parity must win by more than abs_ms.
    CHECK( parity_won(5.0, 16.0, 10.0)); // margin 11 > 10 → true
    CHECK(!parity_won(5.0, 15.0, 10.0)); // margin exactly 10, not > 10 → false
    CHECK(!parity_won(5.0, 14.0, 10.0)); // margin 9 < 10 → false
    CHECK(!parity_won(9.0,  9.5, 10.0)); // margin 0.5 < 10 → false (close race filtered)
    std::puts("PASS test_parity_won");
}

// ─────────────────────────────────────────────────────────────────────────────
int main() {
    test_default_equal_latency();
    test_default_distinct_latency();
    test_ratio_below_threshold();
    test_ratio_at_boundary();
    test_abs_below_threshold();
    test_abs_at_boundary();
    test_and_ratio_fails();
    test_and_both_pass();
    test_even_k_median_interpolation();
    test_single_shard();
    test_parity_won();

    std::puts("All loser_significance tests passed.");
    return 0;
}
