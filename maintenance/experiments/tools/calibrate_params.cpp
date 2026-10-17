// calibrate_params — DiskProfile latency distribution validator
//
// For each of the three canonical HDD scenarios (baseline / mild-slow / severe-slow),
// samples 10,000 latency values from DiskSimulator and prints the resulting
// percentile table to stdout.  No file I/O or sleep is performed.
//
// Usage: ./calibrate_params
// Expected output: each scenario should land within the annotated target ranges.

#include "sim/disk_simulator.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace {

static constexpr int    N_SAMPLES  = 10'000;
static constexpr int    DISK_ID    = 0;
static const std::string TMP_DIR   = "/tmp/healthec_calibrate";

struct ScenarioSpec {
    const char*            name;
    healthec::sim::DiskProfile profile;
    // Expected P50 range [lo, hi] for a quick sanity check
    double p50_lo;
    double p50_hi;
    // Expected P99 range [lo, hi]
    double p99_lo;
    double p99_hi;
};

double percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    double idx = p * (sorted.size() - 1);
    std::size_t lo = static_cast<std::size_t>(idx);
    std::size_t hi = lo + 1;
    if (hi >= sorted.size()) return sorted.back();
    double frac = idx - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

void run_scenario(const ScenarioSpec& spec) {
    healthec::sim::DiskSimulator sim(TMP_DIR, 1, spec.profile, /*seed=*/42);

    std::vector<double> samples;
    samples.reserve(N_SAMPLES);
    for (int i = 0; i < N_SAMPLES; ++i)
        samples.push_back(sim.sample_latency_ms(DISK_ID));

    std::sort(samples.begin(), samples.end());

    double mean = std::accumulate(samples.begin(), samples.end(), 0.0) / samples.size();
    double p1   = percentile(samples, 0.010);
    double p10  = percentile(samples, 0.100);
    double p50  = percentile(samples, 0.500);
    double p90  = percentile(samples, 0.900);
    double p95  = percentile(samples, 0.950);
    double p99  = percentile(samples, 0.990);
    double p999 = percentile(samples, 0.999);
    double vmax = samples.back();

    auto check = [](double val, double lo, double hi) -> const char* {
        return (val >= lo && val <= hi) ? "OK" : "WARN";
    };

    std::cout << "\n[" << spec.name << "]\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  mean  = " << std::setw(8) << mean  << " ms\n";
    std::cout << "  P1    = " << std::setw(8) << p1    << " ms\n";
    std::cout << "  P10   = " << std::setw(8) << p10   << " ms\n";
    std::cout << "  P50   = " << std::setw(8) << p50   << " ms"
              << "  [target " << spec.p50_lo << "–" << spec.p50_hi << "] "
              << check(p50, spec.p50_lo, spec.p50_hi) << "\n";
    std::cout << "  P90   = " << std::setw(8) << p90   << " ms\n";
    std::cout << "  P95   = " << std::setw(8) << p95   << " ms\n";
    std::cout << "  P99   = " << std::setw(8) << p99   << " ms"
              << "  [target " << spec.p99_lo << "–" << spec.p99_hi << "] "
              << check(p99, spec.p99_lo, spec.p99_hi) << "\n";
    std::cout << "  P99.9 = " << std::setw(8) << p999  << " ms\n";
    std::cout << "  max   = " << std::setw(8) << vmax  << " ms\n";
}

}  // namespace

int main() {
    std::filesystem::create_directories(TMP_DIR);

    std::cout << "DiskProfile calibration — " << N_SAMPLES << " samples per scenario\n";
    std::cout << "Seed: 42  (fixed for reproducibility)\n";

    // ── Scenario definitions ──────────────────────────────────────────────────
    // Parameters match experiments/configs/hdd_*.json exactly.
    // Sources: Deep Research Session 4 (research/deep-research-session4-slow-disk-latency-calibration.md)

    using P = healthec::sim::DiskProfile;

    const ScenarioSpec scenarios[] = {
        {
            "Baseline (hdd_baseline.json)",
            P{ .base_mean_ms=9.0, .base_jitter_ms=3.5, .slow_mode=false },
            /*p50*/ 7.0, 11.0,
            /*p99*/ 14.0, 23.0,
        },
        {
            "Mild fail-slow (hdd_mild_slow.json)",
            P{ .base_mean_ms=9.0, .base_jitter_ms=3.5, .slow_mode=true,
               .slow_mean_ms=24.0, .slow_jitter_ms=8.0,
               .spike_prob=0.01, .spike_ms=150.0 },
            /*p50*/ 19.0, 30.0,
            /*p99*/ 38.0, 60.0,
        },
        {
            "Severe fail-slow (hdd_severe_slow.json)",
            P{ .base_mean_ms=9.0, .base_jitter_ms=3.5, .slow_mode=true,
               .slow_mean_ms=55.0, .slow_jitter_ms=18.0,
               .spike_prob=0.03, .spike_ms=500.0 },
            /*p50*/ 45.0, 65.0,
            // spike_prob=0.03 → P97+ are spike-dominated (slow_mean + spike_ms ≈ 555ms)
            /*p99*/ 500.0, 650.0,
        },
    };

    for (const auto& s : scenarios)
        run_scenario(s);

    std::filesystem::remove_all(TMP_DIR);
    std::cout << "\nDone.\n";
    return 0;
}
