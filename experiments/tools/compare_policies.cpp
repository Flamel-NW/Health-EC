// compare_policies - T2 baseline comparison runner
//
// In-memory simulation over the locked T2.2 workload:
//   Vanilla EC / EC-Cache-style late binding / timeout degraded read / Health-EC.
//
// The static canonical_stress20 path intentionally preserves the original
// T2.4.1/T2.4.2 behavior. The dynamic_degradation path precomputes a common
// latent workload and latency trace per seed so policy comparisons do not drift
// because one policy issues more parity reads than another.

#include "core/read_scheduler.h"
#include "core/score_manager.h"
#include "sim/disk_simulator.h"
#include "sim/workload_generator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <unistd.h>

using namespace healthec::core;
using namespace healthec::sim;

// Canonical T2.3/T2.4 workload.

static constexpr int      K                   = 4;
static constexpr int      M                   = 2;
static constexpr int      NUM_DISKS           = 10;
static constexpr int      DEFAULT_NUM_STRIPES = 500;
static constexpr int      DEFAULT_NUM_READS   = 20000;
static constexpr uint64_t DEFAULT_SEED        = 42;
static constexpr double   DEFAULT_ZIPF_S      = 1.0;
static constexpr double   DEFAULT_TIMEOUT_MS  = 30.0;
static constexpr double   DYNAMIC_DEFAULT_TIMEOUT_MS = 15.0;

static constexpr int MILD_DISK   = 8;
static constexpr int SEVERE_DISK = 9;

static constexpr int DYNAMIC_NUM_WINDOWS        = 20;
static constexpr int FIRST_DYNAMIC_ONSET_WINDOW = 3;
static constexpr long NOT_APPLICABLE            = -1;

static const DiskProfile PROFILE_BASELINE{
    .base_mean_ms=9.0, .base_jitter_ms=3.5, .slow_mode=false};

static const DiskProfile PROFILE_MILD{
    .base_mean_ms=9.0, .base_jitter_ms=3.5, .slow_mode=true,
    .slow_mean_ms=24.0, .slow_jitter_ms=8.0, .spike_prob=0.01, .spike_ms=150.0};

static const DiskProfile PROFILE_SEVERE{
    .base_mean_ms=9.0, .base_jitter_ms=3.5, .slow_mode=true,
    .slow_mean_ms=55.0, .slow_jitter_ms=18.0, .spike_prob=0.03, .spike_ms=500.0};

static const DiskProfile PROFILE_RECOVERY{
    .base_mean_ms=9.0, .base_jitter_ms=3.5, .slow_mode=true,
    .slow_mean_ms=13.5, .slow_jitter_ms=5.0, .spike_prob=0.005, .spike_ms=80.0};

// Helpers.

static double percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    double idx = p * static_cast<double>(sorted.size() - 1);
    auto lo = static_cast<std::size_t>(idx);
    auto hi = lo + 1;
    if (hi >= sorted.size()) return sorted.back();
    double frac = idx - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

static double percentile_copy(std::vector<double> values, double p) {
    std::sort(values.begin(), values.end());
    return percentile(values, p);
}

static ShardId make_shard(int stripe, int idx) { return stripe * (K + M) + idx; }

static ScoreParams locked_health_ec_params() {
    ScoreParams p;
    p.theta_S = 1.7;
    p.theta_D = 1.7;
    p.loser_sig_ratio = 0.0;
    p.loser_sig_abs_ms = 15.0;
    p.parity_win_abs_ms = 15.0;
    return p;
}

static void configure_slow_disks(DiskSimulator& sim) {
    sim.set_profile(MILD_DISK, PROFILE_MILD);
    sim.set_profile(SEVERE_DISK, PROFILE_SEVERE);
}

static WorkloadGenerator make_workload(int num_stripes, double zipf_s, uint64_t seed) {
    return WorkloadGenerator(num_stripes, zipf_s, seed);
}

static std::string runtime_tmp_dir(const std::string& prefix, uint64_t seed) {
    auto path = std::filesystem::temp_directory_path() /
                (prefix + "_" + std::to_string(seed) + "_" +
                 std::to_string(static_cast<long long>(::getpid())));
    return path.string();
}

// Layout.

struct Layout {
    std::unordered_map<StripeId, StripeLayout> stripe_layouts;
    std::unordered_map<ShardId, bool> is_slow;
};

static Layout build_layout(int num_stripes) {
    Layout L;
    for (int s = 0; s < num_stripes; ++s) {
        StripeLayout sl;
        const int disk_map_s0[] = {6, 7, MILD_DISK, SEVERE_DISK, 4, 5};
        for (int i = 0; i < K + M; ++i) {
            ShardId sh = make_shard(s, i);
            DiskId dk = (s == 0) ? disk_map_s0[i]
                                 : (s * (K + M) + i) % NUM_DISKS;
            sl.disk_of[sh] = dk;
            if (i < K) sl.data_shards.push_back(sh);
            else sl.parity_shards.push_back(sh);
        }
        L.stripe_layouts[s] = sl;
    }

    for (const auto& [sid, sl] : L.stripe_layouts) {
        (void)sid;
        for (ShardId sh : sl.data_shards) {
            DiskId dk = sl.disk_of.at(sh);
            if (dk == MILD_DISK || dk == SEVERE_DISK)
                L.is_slow[sh] = true;
        }
    }
    return L;
}

// Policy interface.

enum class PolicyKind {
    VanillaEC,
    LateBinding,
    TimeoutDegradedRead,
    HealthEC,
};

enum class PolicySelection {
    All,
    Single,
};

enum class OutputFormat {
    Table,
    Csv,
    WindowedCsv,
    EventTrace,
};

struct RuntimeConfig {
    uint64_t seed = DEFAULT_SEED;
    int num_reads = DEFAULT_NUM_READS;
    int num_stripes = DEFAULT_NUM_STRIPES;
    double zipf_s = DEFAULT_ZIPF_S;
    std::string scenario = "canonical_stress20";
    PolicySelection policy_selection = PolicySelection::All;
    PolicyKind single_policy = PolicyKind::VanillaEC;
    double timeout_ms = DEFAULT_TIMEOUT_MS;
    bool timeout_ms_explicit = false;
    ScoreParams health_ec_params = locked_health_ec_params();
    OutputFormat format = OutputFormat::Table;
};

struct PolicyConfig {
    PolicyKind kind;
    double timeout_ms = 0.0;
    ScoreParams health_ec_params{};
};

struct RunResult {
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    long total_requests = 0;
    long issued_shard_reads = 0;
    long parity_reads = 0;
    long proactive_or_degraded_reads = 0;
    long decode_count = 0;
    long migration_triggers = 0;
    long migration_false_positives = 0;
};

static const char* policy_name(PolicyKind kind) {
    switch (kind) {
        case PolicyKind::VanillaEC: return "vanilla_ec";
        case PolicyKind::LateBinding: return "late_binding";
        case PolicyKind::TimeoutDegradedRead: return "timeout_degraded_read";
        case PolicyKind::HealthEC: return "health_ec";
    }
    return "unknown";
}

static PolicyKind parse_policy_kind(const std::string& value) {
    if (value == "vanilla_ec") return PolicyKind::VanillaEC;
    if (value == "late_binding") return PolicyKind::LateBinding;
    if (value == "timeout_degraded_read") return PolicyKind::TimeoutDegradedRead;
    if (value == "health_ec") return PolicyKind::HealthEC;
    throw std::invalid_argument("invalid --policy: " + value);
}

static int parse_positive_int(const std::string& value, const std::string& name) {
    std::size_t idx = 0;
    long parsed = std::stol(value, &idx, 10);
    if (idx != value.size() || parsed <= 0 ||
        parsed > static_cast<long>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument(name + " must be a positive integer");
    }
    return static_cast<int>(parsed);
}

static uint64_t parse_uint64(const std::string& value, const std::string& name) {
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument(name + " must be an unsigned integer");
    std::size_t idx = 0;
    unsigned long long parsed = std::stoull(value, &idx, 10);
    if (idx != value.size())
        throw std::invalid_argument(name + " must be an unsigned integer");
    return static_cast<uint64_t>(parsed);
}

static double parse_double_value(const std::string& value, const std::string& name) {
    std::size_t idx = 0;
    double parsed = std::stod(value, &idx);
    if (idx != value.size() || !std::isfinite(parsed))
        throw std::invalid_argument(name + " must be a number");
    return parsed;
}

static double parse_nonnegative_double(const std::string& value,
                                       const std::string& name) {
    double parsed = parse_double_value(value, name);
    if (parsed < 0.0)
        throw std::invalid_argument(name + " must be non-negative");
    return parsed;
}

static double parse_positive_double(const std::string& value,
                                    const std::string& name) {
    double parsed = parse_double_value(value, name);
    if (parsed <= 0.0)
        throw std::invalid_argument(name + " must be positive");
    return parsed;
}

static RuntimeConfig parse_args(int argc, char** argv) {
    RuntimeConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto require_value = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc)
                throw std::invalid_argument("missing value for " + name);
            return argv[++i];
        };

        if (arg == "--seed") {
            cfg.seed = parse_uint64(require_value(arg), arg);
        } else if (arg == "--num-reads") {
            cfg.num_reads = parse_positive_int(require_value(arg), arg);
        } else if (arg == "--num-stripes") {
            cfg.num_stripes = parse_positive_int(require_value(arg), arg);
        } else if (arg == "--zipf-s") {
            cfg.zipf_s = parse_double_value(require_value(arg), arg);
            if (cfg.zipf_s <= 0.0)
                throw std::invalid_argument("--zipf-s must be positive");
        } else if (arg == "--scenario") {
            cfg.scenario = require_value(arg);
            if (cfg.scenario != "canonical_stress20" &&
                cfg.scenario != "dynamic_degradation") {
                throw std::invalid_argument("invalid --scenario: " + cfg.scenario);
            }
        } else if (arg == "--policy") {
            std::string value = require_value(arg);
            if (value == "all") {
                cfg.policy_selection = PolicySelection::All;
            } else {
                cfg.policy_selection = PolicySelection::Single;
                cfg.single_policy = parse_policy_kind(value);
            }
        } else if (arg == "--timeout-ms") {
            cfg.timeout_ms = parse_nonnegative_double(require_value(arg), arg);
            cfg.timeout_ms_explicit = true;
        } else if (arg == "--health-theta-s") {
            cfg.health_ec_params.theta_S =
                parse_positive_double(require_value(arg), arg);
        } else if (arg == "--health-theta-d") {
            cfg.health_ec_params.theta_D =
                parse_positive_double(require_value(arg), arg);
        } else if (arg == "--health-loser-sig-abs-ms") {
            cfg.health_ec_params.loser_sig_abs_ms =
                parse_nonnegative_double(require_value(arg), arg);
        } else if (arg == "--health-parity-win-abs-ms") {
            cfg.health_ec_params.parity_win_abs_ms =
                parse_nonnegative_double(require_value(arg), arg);
        } else if (arg == "--format") {
            std::string value = require_value(arg);
            if (value == "table") {
                cfg.format = OutputFormat::Table;
            } else if (value == "csv") {
                cfg.format = OutputFormat::Csv;
            } else if (value == "windowed_csv") {
                cfg.format = OutputFormat::WindowedCsv;
            } else if (value == "event_trace") {
                cfg.format = OutputFormat::EventTrace;
            } else {
                throw std::invalid_argument("invalid --format: " + value);
            }
        } else {
            throw std::invalid_argument("unknown argument: " + arg);
        }
    }
    return cfg;
}

static bool is_dynamic_scenario(const RuntimeConfig& cfg) {
    return cfg.scenario == "dynamic_degradation";
}

static void apply_runtime_defaults(RuntimeConfig& cfg) {
    if (is_dynamic_scenario(cfg) && !cfg.timeout_ms_explicit)
        cfg.timeout_ms = DYNAMIC_DEFAULT_TIMEOUT_MS;
}

static void validate_runtime_config(const RuntimeConfig& cfg) {
    if (!is_dynamic_scenario(cfg) &&
        (cfg.format == OutputFormat::WindowedCsv ||
         cfg.format == OutputFormat::EventTrace)) {
        throw std::invalid_argument(
            "windowed_csv and event_trace require --scenario dynamic_degradation");
    }
    if (is_dynamic_scenario(cfg) && cfg.num_reads % DYNAMIC_NUM_WINDOWS != 0) {
        throw std::invalid_argument(
            "dynamic_degradation requires --num-reads divisible by 20");
    }
}

static std::vector<PolicyKind> selected_policies(const RuntimeConfig& cfg) {
    if (cfg.policy_selection == PolicySelection::Single)
        return {cfg.single_policy};
    return {
        PolicyKind::VanillaEC,
        PolicyKind::LateBinding,
        PolicyKind::TimeoutDegradedRead,
        PolicyKind::HealthEC,
    };
}

static std::vector<std::pair<ShardId, double>> sample_data_latencies(
    DiskSimulator& sim, const StripeLayout& sl)
{
    std::vector<std::pair<ShardId, double>> data_lat;
    data_lat.reserve(sl.data_shards.size());
    for (ShardId sh : sl.data_shards)
        data_lat.emplace_back(sh, sim.sample_latency_ms(sl.disk_of.at(sh)));
    return data_lat;
}

static std::vector<std::pair<ShardId, double>> sorted_with_parity(
    const std::vector<std::pair<ShardId, double>>& data_lat,
    ShardId parity_shard,
    double parity_lat)
{
    auto all = data_lat;
    all.emplace_back(parity_shard, parity_lat);
    std::sort(all.begin(), all.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });
    return all;
}

static RunResult finalize_result(std::vector<double>& latencies, RunResult result) {
    std::sort(latencies.begin(), latencies.end());
    result.p50 = percentile(latencies, 0.50);
    result.p95 = percentile(latencies, 0.95);
    result.p99 = percentile(latencies, 0.99);
    return result;
}

static double bandwidth_overhead_pct(const RunResult& r) {
    double baseline_reads = static_cast<double>(r.total_requests * K);
    return (static_cast<double>(r.issued_shard_reads) - baseline_reads)
         / baseline_reads * 100.0;
}

static double bandwidth_overhead_pct(long issued_shard_reads, long total_requests) {
    double baseline_reads = static_cast<double>(total_requests * K);
    if (baseline_reads <= 0.0) return 0.0;
    return (static_cast<double>(issued_shard_reads) - baseline_reads)
         / baseline_reads * 100.0;
}

static double p99_improvement_pct(double vanilla_p99, const RunResult& r) {
    if (vanilla_p99 <= 0.0) return 0.0;
    return (vanilla_p99 - r.p99) / vanilla_p99 * 100.0;
}

static PolicyConfig make_policy_config(PolicyKind kind,
                                       const RuntimeConfig& runtime,
                                       const ScoreParams& health_params)
{
    PolicyConfig cfg{.kind=kind};
    if (kind == PolicyKind::TimeoutDegradedRead)
        cfg.timeout_ms = runtime.timeout_ms;
    if (kind == PolicyKind::HealthEC)
        cfg.health_ec_params = health_params;
    return cfg;
}

// Static canonical_stress20 runner.

static RunResult run_policy(DiskSimulator& sim, const Layout& layout,
                            WorkloadGenerator& workload,
                            const PolicyConfig& config,
                            int num_reads)
{
    struct ScoreState { double S = 0.0; double D = 0.0; };
    std::unordered_map<ShardId, ScoreState> scores;

    RunResult result;
    result.total_requests = num_reads;

    std::vector<double> latencies;
    latencies.reserve(num_reads);

    for (int r = 0; r < num_reads; ++r) {
        StripeId sid = workload.next_stripe();
        double w_s = workload.hotness(sid);
        const auto& sl = layout.stripe_layouts.at(sid);

        auto data_lat = sample_data_latencies(sim, sl);
        result.issued_shard_reads += K;

        if (config.kind == PolicyKind::VanillaEC) {
            double stripe_lat = 0.0;
            for (const auto& [sh, lat] : data_lat) {
                (void)sh;
                stripe_lat = std::max(stripe_lat, lat);
            }
            latencies.push_back(stripe_lat);
            continue;
        }

        ShardId parity_shard = sl.parity_shards[0];

        if (config.kind == PolicyKind::LateBinding) {
            double parity_lat = sim.sample_latency_ms(sl.disk_of.at(parity_shard));
            result.issued_shard_reads++;
            result.parity_reads++;
            result.proactive_or_degraded_reads++;

            auto all = sorted_with_parity(data_lat, parity_shard, parity_lat);
            latencies.push_back(all[K - 1].second);
            for (int i = 0; i < K; ++i) {
                if (all[i].first == parity_shard) {
                    result.decode_count++;
                    break;
                }
            }
            continue;
        }

        if (config.kind == PolicyKind::TimeoutDegradedRead) {
            std::vector<double> sorted_data;
            sorted_data.reserve(data_lat.size());
            double data_max = 0.0;
            bool timed_out = false;
            for (const auto& [sh, lat] : data_lat) {
                (void)sh;
                sorted_data.push_back(lat);
                data_max = std::max(data_max, lat);
                if (lat > config.timeout_ms) timed_out = true;
            }
            std::sort(sorted_data.begin(), sorted_data.end());

            if (!timed_out) {
                latencies.push_back(data_max);
                continue;
            }

            double parity_lat = sim.sample_latency_ms(sl.disk_of.at(parity_shard));
            result.issued_shard_reads++;
            result.parity_reads++;
            result.proactive_or_degraded_reads++;

            double data_k_minus_1 = sorted_data[K - 2];
            double degraded_lat = std::max(config.timeout_ms + parity_lat,
                                           data_k_minus_1);
            if (degraded_lat < data_max) {
                result.decode_count++;
                latencies.push_back(degraded_lat);
            } else {
                latencies.push_back(data_max);
            }
            continue;
        }

        // Health-EC: stateful Phase A / Phase B loop.
        const ScoreParams& p = config.health_ec_params;
        bool proactive = false;
        for (ShardId sh : sl.data_shards) {
            if (scores[sh].S > p.theta_S) {
                proactive = true;
                break;
            }
        }

        if (!proactive) {
            double stripe_lat = 0.0;
            for (const auto& [sh, lat] : data_lat) {
                (void)sh;
                stripe_lat = std::max(stripe_lat, lat);
            }
            latencies.push_back(stripe_lat);

            auto loser = compute_loser_significant(data_lat, p.loser_sig_ratio,
                                                   p.loser_sig_abs_ms);
            for (const auto& [sh, lat] : data_lat) {
                (void)lat;
                double ev = (loser && sh == *loser) ? 1.0 : 0.0;
                auto& s = scores[sh].S;
                s = (1.0 - p.alpha_S) * s + p.alpha_S * w_s * ev;
            }
        } else {
            result.issued_shard_reads++;
            result.parity_reads++;
            result.proactive_or_degraded_reads++;

            double parity_lat = sim.sample_latency_ms(sl.disk_of.at(parity_shard));
            auto all = sorted_with_parity(data_lat, parity_shard, parity_lat);

            bool parity_in_k = false;
            std::unordered_map<ShardId, bool> in_k;
            for (int i = 0; i < K; ++i) {
                in_k[all[i].first] = true;
                if (all[i].first == parity_shard)
                    parity_in_k = true;
            }
            if (parity_in_k)
                result.decode_count++;
            latencies.push_back(all[K - 1].second);

            for (const auto& [sh, lat] : data_lat) {
                double win = (parity_in_k &&
                              parity_won(parity_lat, lat,
                                         p.parity_win_abs_ms))
                    ? 1.0 : 0.0;

                auto& st = scores[sh];
                st.S = (1.0 - p.alpha_S) * st.S + p.alpha_S * w_s * win;
                st.D = (1.0 - p.alpha_D) * st.D + p.alpha_D * w_s * win;

                if (st.D > p.theta_D) {
                    result.migration_triggers++;
                    if (!layout.is_slow.count(sh))
                        result.migration_false_positives++;
                    st.S = 0.0;
                    st.D = 0.0;
                }
            }
        }
    }

    return finalize_result(latencies, result);
}

static RunResult run_one_policy(const RuntimeConfig& runtime,
                                const Layout& layout,
                                const PolicyConfig& config)
{
    const std::string TMP = runtime_tmp_dir("healthec_compare_policies", runtime.seed);
    DiskSimulator sim(TMP, NUM_DISKS, PROFILE_BASELINE, runtime.seed);
    configure_slow_disks(sim);
    auto workload = make_workload(runtime.num_stripes, runtime.zipf_s, runtime.seed);
    return run_policy(sim, layout, workload, config, runtime.num_reads);
}

// Dynamic degradation model and common latent trace.

enum class DiskState {
    Healthy,
    MildSlow,
    SevereSlow,
    Recovery,
};

struct ScheduleEvent {
    int event_id;
    DiskId disk_id;
    DiskState state;
    int start_window;
    int end_window;
    const char* notes;
};

struct LatentShard {
    ShardId shard;
    DiskId disk;
    bool is_parity;
    DiskState state;
    double latency_ms;
    std::array<double, NUM_DISKS> latency_by_disk{};
};

struct LatentRequest {
    int read_index;
    int window_id;
    StripeId stripe_id;
    double hotness;
    std::vector<LatentShard> data;
    std::vector<LatentShard> parity;
};

struct LatentWorld {
    int num_windows = DYNAMIC_NUM_WINDOWS;
    int window_size = 0;
    std::vector<ScheduleEvent> schedule;
    std::vector<LatentRequest> requests;
};

struct DynamicWindowResult {
    int window_id = 0;
    int window_start_read = 0;
    int window_end_read = 0;
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    long total_requests = 0;
    long issued_shard_reads = 0;
    long parity_reads = 0;
    long proactive_or_degraded_reads = 0;
    long decode_count = 0;
    long migration_triggers = 0;
    long migration_true_positives = 0;
    long migration_false_positives = 0;
    std::vector<double> latencies;
};

struct DynamicRunResult {
    RunResult aggregate;
    std::vector<double> aggregate_latencies;
    std::vector<DynamicWindowResult> windows;
    double post_onset_p95_auc_ms = 0.0;
    double post_onset_p99_auc_ms = 0.0;
    long pre_slowdown_parity_reads = 0;
    long recovery_parity_reads = 0;
    long migration_true_positives = 0;
    long migration_false_negatives = NOT_APPLICABLE;
    long first_detection_latency_reads = NOT_APPLICABLE;
    long first_mitigation_latency_reads = NOT_APPLICABLE;
    long recovery_regret_reads = 0;
};

struct EventShardStats {
    int read_count = 0;
    bool migrated = false;
    bool trigger_recorded = false;
};

static bool is_migration_positive(DiskState state) {
    return state == DiskState::MildSlow || state == DiskState::SevereSlow;
}

static const char* disk_state_name(DiskState state) {
    switch (state) {
        case DiskState::Healthy: return "healthy";
        case DiskState::MildSlow: return "mild_slow";
        case DiskState::SevereSlow: return "severe_slow";
        case DiskState::Recovery: return "recovery";
    }
    return "unknown";
}

static DiskProfile profile_for_state(DiskState state) {
    switch (state) {
        case DiskState::Healthy: return PROFILE_BASELINE;
        case DiskState::MildSlow: return PROFILE_MILD;
        case DiskState::SevereSlow: return PROFILE_SEVERE;
        case DiskState::Recovery: return PROFILE_RECOVERY;
    }
    return PROFILE_BASELINE;
}

static std::vector<ScheduleEvent> dynamic_schedule() {
    return {
        {0, MILD_DISK,   DiskState::Healthy,    0,  3, "warmup_before_first_onset"},
        {1, MILD_DISK,   DiskState::MildSlow,   3,  6, "first_gradual_degradation"},
        {2, MILD_DISK,   DiskState::SevereSlow, 6,  9, "sustained_severe_period"},
        {3, MILD_DISK,   DiskState::Recovery,   9, 11, "partial_recovery"},
        {4, MILD_DISK,   DiskState::Healthy,   11, 14, "recovered_interval"},
        {5, MILD_DISK,   DiskState::MildSlow,  14, 15, "relapse"},
        {6, MILD_DISK,   DiskState::Recovery,  15, 16, "relapse_recovery"},
        {7, MILD_DISK,   DiskState::Healthy,   16, 20, "post_recovery_observation"},
        {8, SEVERE_DISK, DiskState::Healthy,    0,  9, "staggered_later_onset"},
        {9, SEVERE_DISK, DiskState::MildSlow,   9, 11, "second_disk_mild_period"},
        {10, SEVERE_DISK, DiskState::SevereSlow, 11, 14, "second_disk_severe_period"},
        {11, SEVERE_DISK, DiskState::Recovery,  14, 16, "second_disk_recovery"},
        {12, SEVERE_DISK, DiskState::Healthy,   16, 20, "second_disk_post_recovery"},
    };
}

static const ScheduleEvent* event_for_disk_window(
    const std::vector<ScheduleEvent>& schedule, DiskId disk, int window_id)
{
    for (const auto& e : schedule) {
        if (e.disk_id == disk &&
            window_id >= e.start_window &&
            window_id < e.end_window) {
            return &e;
        }
    }
    return nullptr;
}

static DiskState state_for_disk_window(
    const std::vector<ScheduleEvent>& schedule, DiskId disk, int window_id)
{
    const ScheduleEvent* e = event_for_disk_window(schedule, disk, window_id);
    return e ? e->state : DiskState::Healthy;
}

static bool disk_has_prior_positive_end(
    const std::vector<ScheduleEvent>& schedule, DiskId disk, int window_id)
{
    for (const auto& e : schedule) {
        if (e.disk_id == disk &&
            is_migration_positive(e.state) &&
            e.end_window <= window_id) {
            return true;
        }
    }
    return false;
}

static bool is_post_slow_shard(
    DiskId disk, DiskState state, const LatentWorld& world, int window_id)
{
    return !is_migration_positive(state) &&
           disk_has_prior_positive_end(world.schedule, disk, window_id);
}

static DiskId logical_disk_for(const std::unordered_map<ShardId, DiskId>& logical_disk,
                               const LatentShard& shard)
{
    auto it = logical_disk.find(shard.shard);
    return (it != logical_disk.end()) ? it->second : shard.disk;
}

static double latency_on_disk(const LatentShard& shard, DiskId disk) {
    if (disk < 0 || disk >= NUM_DISKS)
        throw std::out_of_range("latency_on_disk: disk out of range");
    return shard.latency_by_disk[disk];
}

static std::unordered_map<ShardId, DiskId> initial_logical_disks(
    const Layout& layout)
{
    std::unordered_map<ShardId, DiskId> out;
    for (const auto& [sid, sl] : layout.stripe_layouts) {
        (void)sid;
        for (const auto& [sh, disk] : sl.disk_of)
            out[sh] = disk;
    }
    return out;
}

static bool request_has_positive_data(
    const LatentWorld& world,
    const LatentRequest& req,
    const std::unordered_map<ShardId, DiskId>& logical_disk)
{
    for (const auto& sh : req.data) {
        DiskId disk = logical_disk_for(logical_disk, sh);
        if (is_migration_positive(
                state_for_disk_window(world.schedule, disk, req.window_id))) {
            return true;
        }
    }
    return false;
}

static bool request_has_recovery_data(
    const LatentWorld& world,
    const LatentRequest& req,
    const std::unordered_map<ShardId, DiskId>& logical_disk)
{
    for (const auto& sh : req.data) {
        DiskId disk = logical_disk_for(logical_disk, sh);
        if (state_for_disk_window(world.schedule, disk, req.window_id) ==
            DiskState::Recovery) {
            return true;
        }
    }
    return false;
}

static bool request_has_post_slow_data(const LatentRequest& req,
                                       const LatentWorld& world,
                                       const std::unordered_map<ShardId, DiskId>& logical_disk) {
    for (const auto& sh : req.data) {
        DiskId disk = logical_disk_for(logical_disk, sh);
        DiskState state = state_for_disk_window(world.schedule, disk, req.window_id);
        if (is_post_slow_shard(disk, state, world, req.window_id)) return true;
    }
    return false;
}

static int active_slow_disks_for_window(
    const std::vector<ScheduleEvent>& schedule, int window_id)
{
    int count = 0;
    if (is_migration_positive(state_for_disk_window(schedule, MILD_DISK, window_id)))
        count++;
    if (is_migration_positive(state_for_disk_window(schedule, SEVERE_DISK, window_id)))
        count++;
    return count;
}

static LatentWorld build_dynamic_world(const RuntimeConfig& runtime,
                                       const Layout& layout)
{
    LatentWorld world;
    world.window_size = runtime.num_reads / DYNAMIC_NUM_WINDOWS;
    world.schedule = dynamic_schedule();
    world.requests.reserve(runtime.num_reads);

    const std::string TMP =
        runtime_tmp_dir("healthec_compare_policies_dynamic_trace", runtime.seed);
    std::filesystem::remove_all(TMP);
    DiskSimulator sim(TMP, NUM_DISKS, PROFILE_BASELINE, runtime.seed);
    auto workload = make_workload(runtime.num_stripes, runtime.zipf_s, runtime.seed);

    for (int r = 0; r < runtime.num_reads; ++r) {
        StripeId sid = workload.next_stripe();
        int window_id = r / world.window_size;
        const auto& sl = layout.stripe_layouts.at(sid);

        LatentRequest req;
        req.read_index = r;
        req.window_id = window_id;
        req.stripe_id = sid;
        req.hotness = workload.hotness(sid);
        req.data.reserve(sl.data_shards.size());
        req.parity.reserve(sl.parity_shards.size());

        for (ShardId sh : sl.data_shards) {
            DiskId disk = sl.disk_of.at(sh);
            DiskState state = state_for_disk_window(world.schedule, disk, window_id);
            LatentShard latent{.shard=sh, .disk=disk, .is_parity=false,
                                .state=state, .latency_ms=0.0};
            for (DiskId d = 0; d < NUM_DISKS; ++d) {
                DiskState disk_state = state_for_disk_window(world.schedule, d, window_id);
                sim.set_profile(d, profile_for_state(disk_state));
                latent.latency_by_disk[d] = sim.sample_latency_ms(d);
            }
            latent.latency_ms = latent.latency_by_disk[disk];
            req.data.push_back(std::move(latent));
        }
        for (ShardId sh : sl.parity_shards) {
            DiskId disk = sl.disk_of.at(sh);
            DiskState state = state_for_disk_window(world.schedule, disk, window_id);
            LatentShard latent{.shard=sh, .disk=disk, .is_parity=true,
                                .state=state, .latency_ms=0.0};
            for (DiskId d = 0; d < NUM_DISKS; ++d) {
                DiskState disk_state = state_for_disk_window(world.schedule, d, window_id);
                sim.set_profile(d, profile_for_state(disk_state));
                latent.latency_by_disk[d] = sim.sample_latency_ms(d);
            }
            latent.latency_ms = latent.latency_by_disk[disk];
            req.parity.push_back(std::move(latent));
        }

        world.requests.push_back(std::move(req));
    }

    std::filesystem::remove_all(TMP);
    return world;
}

static std::vector<std::pair<ShardId, double>> latent_data_latencies(
    const LatentRequest& req,
    const std::unordered_map<ShardId, DiskId>& logical_disk)
{
    std::vector<std::pair<ShardId, double>> out;
    out.reserve(req.data.size());
    for (const auto& sh : req.data) {
        DiskId disk = logical_disk_for(logical_disk, sh);
        out.emplace_back(sh.shard, latency_on_disk(sh, disk));
    }
    return out;
}

static const LatentShard* find_data_shard(const LatentRequest& req, ShardId shard) {
    for (const auto& sh : req.data)
        if (sh.shard == shard) return &sh;
    return nullptr;
}

static const LatentShard* slowest_data_shard(const LatentRequest& req) {
    if (req.data.empty()) return nullptr;
    const LatentShard* slowest = &req.data.front();
    for (const auto& sh : req.data)
        if (sh.latency_ms > slowest->latency_ms) slowest = &sh;
    return slowest;
}

static bool positive_data_bypassed_by_parity(
    const LatentWorld& world,
    const LatentRequest& req,
    const std::unordered_map<ShardId, DiskId>& logical_disk,
    const std::unordered_map<ShardId, bool>& in_k,
    bool parity_in_k)
{
    if (!parity_in_k) return false;
    for (const auto& sh : req.data) {
        DiskId disk = logical_disk_for(logical_disk, sh);
        DiskState state = state_for_disk_window(world.schedule, disk, req.window_id);
        if (is_migration_positive(state) && !in_k.count(sh.shard))
            return true;
    }
    return false;
}

static std::vector<DynamicWindowResult> make_dynamic_windows(const LatentWorld& world) {
    std::vector<DynamicWindowResult> windows(world.num_windows);
    for (int w = 0; w < world.num_windows; ++w) {
        windows[w].window_id = w;
        windows[w].window_start_read = w * world.window_size;
        windows[w].window_end_read = (w + 1) * world.window_size;
        windows[w].latencies.reserve(world.window_size);
    }
    return windows;
}

static void record_data_reads(DynamicRunResult& result, const LatentRequest& req) {
    result.aggregate.issued_shard_reads += K;
    auto& win = result.windows.at(req.window_id);
    win.total_requests++;
    win.issued_shard_reads += K;
}

static void record_extra_parity_read(DynamicRunResult& result,
                                     const LatentWorld& world,
                                     const LatentRequest& req,
                                     const std::unordered_map<ShardId, DiskId>& logical_disk)
{
    result.aggregate.issued_shard_reads++;
    result.aggregate.parity_reads++;
    result.aggregate.proactive_or_degraded_reads++;

    auto& win = result.windows.at(req.window_id);
    win.issued_shard_reads++;
    win.parity_reads++;
    win.proactive_or_degraded_reads++;

    if (req.window_id < FIRST_DYNAMIC_ONSET_WINDOW)
        result.pre_slowdown_parity_reads++;
    if (request_has_recovery_data(world, req, logical_disk))
        result.recovery_parity_reads++;
    if (request_has_post_slow_data(req, world, logical_disk))
        result.recovery_regret_reads++;
}

static void record_decode(DynamicRunResult& result, const LatentRequest& req) {
    result.aggregate.decode_count++;
    result.windows.at(req.window_id).decode_count++;
}

static void record_latency(DynamicRunResult& result,
                           const LatentRequest& req,
                           double latency_ms)
{
    result.aggregate_latencies.push_back(latency_ms);
    result.windows.at(req.window_id).latencies.push_back(latency_ms);
}

static void update_observed_health(std::array<double, NUM_DISKS>& disk_health,
                                   DiskId disk,
                                   double latency_ms,
                                   const ScoreParams& params)
{
    double normalized_latency = std::min(std::max(latency_ms / 100.0, 0.0), 1.0);
    double sample_health = 1.0 - normalized_latency;
    disk_health[disk] = (1.0 - params.alpha_H) * disk_health[disk] +
                        params.alpha_H * sample_health;
}

static DiskId select_migration_target(
    const LatentRequest& req,
    const std::unordered_map<ShardId, DiskId>& logical_disk,
    const std::array<double, NUM_DISKS>& disk_health,
    DiskId current_disk)
{
    std::array<bool, NUM_DISKS> used{};
    for (const auto& sh : req.data)
        used[logical_disk_for(logical_disk, sh)] = true;
    for (const auto& sh : req.parity)
        used[sh.disk] = true;

    used[current_disk] = false;
    DiskId target = -1;
    double target_health = disk_health[current_disk];
    for (DiskId d = 0; d < NUM_DISKS; ++d) {
        if (used[d]) continue;
        if (disk_health[d] > target_health) {
            target = d;
            target_health = disk_health[d];
        }
    }
    return target;
}

static void record_migration_trigger(
    DynamicRunResult& result,
    const LatentWorld& world,
    const LatentRequest& req,
    const LatentShard& shard,
    DiskId current_disk,
    std::unordered_map<int, std::unordered_map<ShardId, EventShardStats>>& event_stats)
{
    result.aggregate.migration_triggers++;
    result.windows.at(req.window_id).migration_triggers++;

    const ScheduleEvent* e = event_for_disk_window(
        world.schedule, current_disk, req.window_id);
    DiskState state = e ? e->state : DiskState::Healthy;
    int event_key = e ? e->event_id : (-1000 - current_disk);
    auto& stats = event_stats[event_key][shard.shard];

    if (!stats.trigger_recorded) {
        stats.trigger_recorded = true;
        if (is_migration_positive(state)) {
            result.migration_true_positives++;
            result.windows.at(req.window_id).migration_true_positives++;
        } else {
            result.aggregate.migration_false_positives++;
            result.windows.at(req.window_id).migration_false_positives++;
        }
    }
    if (is_migration_positive(state))
        stats.migrated = true;

    if (is_post_slow_shard(current_disk, state, world, req.window_id))
        result.recovery_regret_reads++;
}

static void record_positive_event_reads(
    const LatentWorld& world,
    const LatentRequest& req,
    const std::unordered_map<ShardId, DiskId>& logical_disk,
    std::unordered_map<int, std::unordered_map<ShardId, EventShardStats>>& event_stats)
{
    for (const auto& sh : req.data) {
        DiskId disk = logical_disk_for(logical_disk, sh);
        const ScheduleEvent* e = event_for_disk_window(
            world.schedule, disk, req.window_id);
        if (e && is_migration_positive(e->state))
            event_stats[e->event_id][sh.shard].read_count++;
    }
}

static long compute_migration_false_negatives(
    const std::unordered_map<int, std::unordered_map<ShardId, EventShardStats>>& event_stats)
{
    long fn = 0;
    for (const auto& [event_id, shards] : event_stats) {
        (void)event_id;
        for (const auto& [shard, stats] : shards) {
            (void)shard;
            if (stats.read_count >= 5 && !stats.migrated)
                fn++;
        }
    }
    return fn;
}

static void finalize_dynamic_result(DynamicRunResult& result,
                                    const LatentWorld& world)
{
    result.aggregate.total_requests = static_cast<long>(world.requests.size());
    result.aggregate = finalize_result(result.aggregate_latencies, result.aggregate);

    for (auto& win : result.windows) {
        std::sort(win.latencies.begin(), win.latencies.end());
        win.p50 = percentile(win.latencies, 0.50);
        win.p95 = percentile(win.latencies, 0.95);
        win.p99 = percentile(win.latencies, 0.99);
    }

    std::vector<double> warmup_p95;
    std::vector<double> warmup_p99;
    for (int w = 0; w < FIRST_DYNAMIC_ONSET_WINDOW; ++w) {
        warmup_p95.push_back(result.windows.at(w).p95);
        warmup_p99.push_back(result.windows.at(w).p99);
    }
    double baseline_p95 = percentile_copy(warmup_p95, 0.50);
    double baseline_p99 = percentile_copy(warmup_p99, 0.50);

    for (int w = FIRST_DYNAMIC_ONSET_WINDOW; w < world.num_windows; ++w) {
        result.post_onset_p95_auc_ms +=
            std::max(result.windows.at(w).p95 - baseline_p95, 0.0);
        result.post_onset_p99_auc_ms +=
            std::max(result.windows.at(w).p99 - baseline_p99, 0.0);
    }
}

static DynamicRunResult run_dynamic_policy(const LatentWorld& world,
                                           const Layout& layout,
                                           const PolicyConfig& config)
{
    struct ScoreState { double S = 0.0; double D = 0.0; };
    std::unordered_map<ShardId, ScoreState> scores;
    std::unordered_map<ShardId, DiskId> logical_disk = initial_logical_disks(layout);
    std::array<double, NUM_DISKS> disk_health{};
    disk_health.fill(1.0);
    std::unordered_map<int, std::unordered_map<ShardId, EventShardStats>> event_stats;

    DynamicRunResult result;
    result.windows = make_dynamic_windows(world);
    result.aggregate_latencies.reserve(world.requests.size());
    if (config.kind != PolicyKind::HealthEC)
        result.migration_false_negatives = NOT_APPLICABLE;

    const int first_onset_read = FIRST_DYNAMIC_ONSET_WINDOW * world.window_size;

    for (const auto& req : world.requests) {
        record_data_reads(result, req);
        if (config.kind == PolicyKind::HealthEC)
            record_positive_event_reads(world, req, logical_disk, event_stats);

        auto data_lat = latent_data_latencies(req, logical_disk);
        ShardId parity_shard = req.parity.front().shard;
        DiskId parity_disk = logical_disk_for(logical_disk, req.parity.front());
        double parity_lat = latency_on_disk(req.parity.front(), parity_disk);

        if (config.kind == PolicyKind::HealthEC) {
            for (const auto& sh : req.data) {
                DiskId disk = logical_disk_for(logical_disk, sh);
                update_observed_health(disk_health, disk,
                                       latency_on_disk(sh, disk),
                                       config.health_ec_params);
            }
        }

        if (config.kind == PolicyKind::VanillaEC) {
            double stripe_lat = 0.0;
            for (const auto& [sh, lat] : data_lat) {
                (void)sh;
                stripe_lat = std::max(stripe_lat, lat);
            }
            record_latency(result, req, stripe_lat);
            continue;
        }

        if (config.kind == PolicyKind::LateBinding) {
            record_extra_parity_read(result, world, req, logical_disk);
            auto all = sorted_with_parity(data_lat, parity_shard, parity_lat);

            bool parity_in_k = false;
            std::unordered_map<ShardId, bool> in_k;
            for (int i = 0; i < K; ++i) {
                in_k[all[i].first] = true;
                if (all[i].first == parity_shard)
                    parity_in_k = true;
            }
            if (parity_in_k)
                record_decode(result, req);
            if (req.read_index >= first_onset_read &&
                result.first_mitigation_latency_reads < 0 &&
                positive_data_bypassed_by_parity(
                    world, req, logical_disk, in_k, parity_in_k)) {
                result.first_mitigation_latency_reads =
                    req.read_index - first_onset_read;
            }

            record_latency(result, req, all[K - 1].second);
            continue;
        }

        if (config.kind == PolicyKind::TimeoutDegradedRead) {
            std::vector<double> sorted_data;
            sorted_data.reserve(data_lat.size());
            double data_max = 0.0;
            bool timed_out = false;
            for (const auto& [sh, lat] : data_lat) {
                (void)sh;
                sorted_data.push_back(lat);
                data_max = std::max(data_max, lat);
                if (lat > config.timeout_ms) timed_out = true;
            }
            std::sort(sorted_data.begin(), sorted_data.end());

            if (!timed_out) {
                record_latency(result, req, data_max);
                continue;
            }

            record_extra_parity_read(result, world, req, logical_disk);
            if (req.read_index >= first_onset_read &&
                request_has_positive_data(world, req, logical_disk) &&
                result.first_detection_latency_reads < 0) {
                result.first_detection_latency_reads =
                    req.read_index - first_onset_read;
            }

            double data_k_minus_1 = sorted_data[K - 2];
            double degraded_lat = std::max(config.timeout_ms + parity_lat,
                                           data_k_minus_1);
            if (degraded_lat < data_max) {
                record_decode(result, req);
                const LatentShard* slowest = slowest_data_shard(req);
                if (slowest && is_migration_positive(slowest->state) &&
                    req.read_index >= first_onset_read &&
                    result.first_mitigation_latency_reads < 0) {
                    result.first_mitigation_latency_reads =
                        req.read_index - first_onset_read;
                }
                record_latency(result, req, degraded_lat);
            } else {
                record_latency(result, req, data_max);
            }
            continue;
        }

        const ScoreParams& p = config.health_ec_params;
        bool proactive = false;
        for (const auto& [sh, lat] : data_lat) {
            (void)lat;
            if (scores[sh].S > p.theta_S) {
                proactive = true;
                break;
            }
        }

        if (!proactive) {
            double stripe_lat = 0.0;
            for (const auto& [sh, lat] : data_lat) {
                (void)sh;
                stripe_lat = std::max(stripe_lat, lat);
            }
            record_latency(result, req, stripe_lat);

            auto loser = compute_loser_significant(data_lat, p.loser_sig_ratio,
                                                   p.loser_sig_abs_ms);
            for (const auto& [sh, lat] : data_lat) {
                (void)lat;
                double ev = (loser && sh == *loser) ? 1.0 : 0.0;
                auto& s = scores[sh].S;
                s = (1.0 - p.alpha_S) * s + p.alpha_S * req.hotness * ev;
            }
            continue;
        }

        record_extra_parity_read(result, world, req, logical_disk);
        update_observed_health(disk_health, parity_disk, parity_lat, p);
        if (req.read_index >= first_onset_read &&
            request_has_positive_data(world, req, logical_disk) &&
            result.first_detection_latency_reads < 0) {
            result.first_detection_latency_reads = req.read_index - first_onset_read;
        }

        auto all = sorted_with_parity(data_lat, parity_shard, parity_lat);

        bool parity_in_k = false;
        std::unordered_map<ShardId, bool> in_k;
        for (int i = 0; i < K; ++i) {
            in_k[all[i].first] = true;
            if (all[i].first == parity_shard)
                parity_in_k = true;
        }
        if (parity_in_k)
            record_decode(result, req);
        if (req.read_index >= first_onset_read &&
            result.first_mitigation_latency_reads < 0 &&
            positive_data_bypassed_by_parity(
                world, req, logical_disk, in_k, parity_in_k)) {
            result.first_mitigation_latency_reads =
                req.read_index - first_onset_read;
        }

        record_latency(result, req, all[K - 1].second);

        for (const auto& [sh, lat] : data_lat) {
            double win = (parity_in_k &&
                          parity_won(parity_lat, lat,
                                     p.parity_win_abs_ms))
                ? 1.0 : 0.0;

            auto& st = scores[sh];
            st.S = (1.0 - p.alpha_S) * st.S + p.alpha_S * req.hotness * win;
            st.D = (1.0 - p.alpha_D) * st.D + p.alpha_D * req.hotness * win;

            if (st.D > p.theta_D) {
                const LatentShard* data_shard = find_data_shard(req, sh);
                if (data_shard) {
                    DiskId current_disk = logical_disk_for(logical_disk, *data_shard);
                    DiskId target = select_migration_target(
                        req, logical_disk, disk_health, current_disk);
                    if (target == -1)
                        continue;
                    record_migration_trigger(
                        result, world, req, *data_shard, current_disk, event_stats);
                    logical_disk[sh] = target;
                }
                st.S = 0.0;
                st.D = 0.0;
            }
        }
    }

    if (config.kind == PolicyKind::HealthEC)
        result.migration_false_negatives =
            compute_migration_false_negatives(event_stats);

    finalize_dynamic_result(result, world);
    return result;
}

// Output.

static void print_table_header(const RuntimeConfig& runtime,
                               const ScoreParams& health_params)
{
    std::cout << "compare_policies"
              << "  scenario=" << runtime.scenario
              << "  seed=" << runtime.seed
              << "  reads=" << runtime.num_reads
              << "  stripes=" << runtime.num_stripes
              << "  zipf_s=" << runtime.zipf_s << "\n";
    if (is_dynamic_scenario(runtime)) {
        std::cout << "Dynamic schedule: 20 windows; disk" << MILD_DISK
                  << " first onset; disk" << SEVERE_DISK
                  << " staggered onset; recovery/relapse included\n";
    } else {
        std::cout << "Slow disks: disk" << MILD_DISK << "=mild  disk"
                  << SEVERE_DISK << "=severe  ratio=20% stress\n";
    }
    std::cout << "Timeout baseline: timeout_ms=" << runtime.timeout_ms << "\n";
    std::cout << "Health-EC locked params: theta_S=" << health_params.theta_S
              << " theta_D=" << health_params.theta_D
              << " loser_sig_abs_ms=" << health_params.loser_sig_abs_ms
              << " parity_win_abs_ms=" << health_params.parity_win_abs_ms
              << "\n\n";

    std::cout << std::left
              << std::setw(24) << "policy"
              << std::right
              << std::setw(9) << "P50"
              << std::setw(9) << "P95"
              << std::setw(9) << "P99"
              << std::setw(10) << "P99imp%"
              << std::setw(12) << "reads"
              << std::setw(9) << "BW%"
              << std::setw(9) << "parity"
              << std::setw(12) << "pro/deg"
              << std::setw(9) << "decode"
              << std::setw(9) << "mig"
              << std::setw(9) << "migFP"
              << "\n";
    std::cout << std::string(130, '-') << "\n";
}

static void print_table_row(PolicyKind kind, const RunResult& r,
                            double vanilla_p99)
{
    std::cout << std::fixed << std::setprecision(1)
              << std::left << std::setw(24) << policy_name(kind)
              << std::right
              << std::setw(9) << r.p50
              << std::setw(9) << r.p95
              << std::setw(9) << r.p99
              << std::setw(10) << p99_improvement_pct(vanilla_p99, r)
              << std::setw(12) << r.issued_shard_reads
              << std::setw(9) << bandwidth_overhead_pct(r)
              << std::setw(9) << r.parity_reads
              << std::setw(12) << r.proactive_or_degraded_reads
              << std::setw(9) << r.decode_count
              << std::setw(9) << r.migration_triggers
              << std::setw(9) << r.migration_false_positives
              << "\n";
}

static void print_static_csv_header()
{
    std::cout
        << "scenario,seed,num_reads,num_stripes,zipf_s,policy,timeout_ms,"
        << "p50_ms,p95_ms,p99_ms,p99_improvement_pct,issued_shard_reads,"
        << "bandwidth_overhead_pct,parity_reads,proactive_or_degraded_reads,"
        << "decode_count,migration_triggers,migration_false_positives\n";
}

static void print_static_csv_row(const RuntimeConfig& runtime,
                                 PolicyKind kind,
                                 const RunResult& r,
                                 double vanilla_p99)
{
    std::cout << std::fixed << std::setprecision(1)
              << runtime.scenario << ','
              << runtime.seed << ','
              << runtime.num_reads << ','
              << runtime.num_stripes << ','
              << runtime.zipf_s << ','
              << policy_name(kind) << ','
              << runtime.timeout_ms << ','
              << r.p50 << ','
              << r.p95 << ','
              << r.p99 << ','
              << p99_improvement_pct(vanilla_p99, r) << ','
              << r.issued_shard_reads << ','
              << bandwidth_overhead_pct(r) << ','
              << r.parity_reads << ','
              << r.proactive_or_degraded_reads << ','
              << r.decode_count << ','
              << r.migration_triggers << ','
              << r.migration_false_positives << "\n";
}

static void print_dynamic_csv_header()
{
    std::cout
        << "scenario,seed,num_reads,num_stripes,zipf_s,num_windows,window_size,"
        << "policy,timeout_ms,p50_ms,p95_ms,p99_ms,p99_improvement_pct,"
        << "post_onset_p95_auc_ms,post_onset_p99_auc_ms,issued_shard_reads,"
        << "bandwidth_overhead_pct,parity_reads,pre_slowdown_parity_reads,"
        << "recovery_parity_reads,proactive_or_degraded_reads,decode_count,"
        << "migration_triggers,migration_true_positives,migration_false_positives,"
        << "migration_false_negatives,first_detection_latency_reads,"
        << "first_mitigation_latency_reads,recovery_regret_reads\n";
}

static void print_dynamic_csv_row(const RuntimeConfig& runtime,
                                  const LatentWorld& world,
                                  PolicyKind kind,
                                  const DynamicRunResult& r,
                                  double vanilla_p99)
{
    std::cout << std::fixed << std::setprecision(1)
              << runtime.scenario << ','
              << runtime.seed << ','
              << runtime.num_reads << ','
              << runtime.num_stripes << ','
              << runtime.zipf_s << ','
              << world.num_windows << ','
              << world.window_size << ','
              << policy_name(kind) << ','
              << runtime.timeout_ms << ','
              << r.aggregate.p50 << ','
              << r.aggregate.p95 << ','
              << r.aggregate.p99 << ','
              << p99_improvement_pct(vanilla_p99, r.aggregate) << ','
              << r.post_onset_p95_auc_ms << ','
              << r.post_onset_p99_auc_ms << ','
              << r.aggregate.issued_shard_reads << ','
              << bandwidth_overhead_pct(r.aggregate) << ','
              << r.aggregate.parity_reads << ','
              << r.pre_slowdown_parity_reads << ','
              << r.recovery_parity_reads << ','
              << r.aggregate.proactive_or_degraded_reads << ','
              << r.aggregate.decode_count << ','
              << r.aggregate.migration_triggers << ','
              << r.migration_true_positives << ','
              << r.aggregate.migration_false_positives << ','
              << r.migration_false_negatives << ','
              << r.first_detection_latency_reads << ','
              << r.first_mitigation_latency_reads << ','
              << r.recovery_regret_reads << "\n";
}

static void print_windowed_csv_header()
{
    std::cout
        << "scenario,seed,num_reads,num_stripes,zipf_s,policy,timeout_ms,"
        << "window_id,window_start_read,window_end_read,disk8_state,disk9_state,"
        << "active_slow_disks,p50_ms,p95_ms,p99_ms,issued_shard_reads,"
        << "bandwidth_overhead_pct,parity_reads,proactive_or_degraded_reads,"
        << "decode_count,migration_triggers,migration_true_positives,"
        << "migration_false_positives\n";
}

static void print_windowed_csv_rows(const RuntimeConfig& runtime,
                                    const LatentWorld& world,
                                    PolicyKind kind,
                                    const DynamicRunResult& result)
{
    for (const auto& win : result.windows) {
        std::cout << std::fixed << std::setprecision(1)
                  << runtime.scenario << ','
                  << runtime.seed << ','
                  << runtime.num_reads << ','
                  << runtime.num_stripes << ','
                  << runtime.zipf_s << ','
                  << policy_name(kind) << ','
                  << runtime.timeout_ms << ','
                  << win.window_id << ','
                  << win.window_start_read << ','
                  << win.window_end_read << ','
                  << disk_state_name(state_for_disk_window(
                         world.schedule, MILD_DISK, win.window_id)) << ','
                  << disk_state_name(state_for_disk_window(
                         world.schedule, SEVERE_DISK, win.window_id)) << ','
                  << active_slow_disks_for_window(world.schedule, win.window_id) << ','
                  << win.p50 << ','
                  << win.p95 << ','
                  << win.p99 << ','
                  << win.issued_shard_reads << ','
                  << bandwidth_overhead_pct(win.issued_shard_reads,
                                            win.total_requests) << ','
                  << win.parity_reads << ','
                  << win.proactive_or_degraded_reads << ','
                  << win.decode_count << ','
                  << win.migration_triggers << ','
                  << win.migration_true_positives << ','
                  << win.migration_false_positives << "\n";
    }
}

static void print_event_trace(const RuntimeConfig& runtime)
{
    int window_size = runtime.num_reads / DYNAMIC_NUM_WINDOWS;
    auto schedule = dynamic_schedule();
    std::cout
        << "scenario,seed,event_id,disk_id,state,start_window,end_window,"
        << "start_read,end_read,is_migration_positive,notes\n";
    for (const auto& e : schedule) {
        std::cout << runtime.scenario << ','
                  << runtime.seed << ','
                  << e.event_id << ','
                  << e.disk_id << ','
                  << disk_state_name(e.state) << ','
                  << e.start_window << ','
                  << e.end_window << ','
                  << e.start_window * window_size << ','
                  << e.end_window * window_size << ','
                  << (is_migration_positive(e.state) ? 1 : 0) << ','
                  << e.notes << "\n";
    }
}

// main.

int main(int argc, char** argv) {
    RuntimeConfig runtime;
    try {
        runtime = parse_args(argc, argv);
        apply_runtime_defaults(runtime);
        validate_runtime_config(runtime);
    } catch (const std::exception& e) {
        std::cerr << "compare_policies: " << e.what() << "\n";
        return 1;
    }

    const ScoreParams health_params = runtime.health_ec_params;
    const auto policies = selected_policies(runtime);

    if (is_dynamic_scenario(runtime) && runtime.format == OutputFormat::EventTrace) {
        print_event_trace(runtime);
        return 0;
    }

    const std::string TMP = runtime_tmp_dir("healthec_compare_policies", runtime.seed);
    std::filesystem::remove_all(TMP);
    std::filesystem::create_directories(TMP);

    const Layout layout = build_layout(runtime.num_stripes);

    if (is_dynamic_scenario(runtime)) {
        const LatentWorld world = build_dynamic_world(runtime, layout);
        std::vector<PolicyConfig> configs;
        std::vector<DynamicRunResult> results;
        configs.reserve(policies.size());
        results.reserve(policies.size());

        for (PolicyKind kind : policies) {
            configs.push_back(make_policy_config(kind, runtime, health_params));
            results.push_back(run_dynamic_policy(world, layout, configs.back()));
        }

        double vanilla_p99 = 0.0;
        if (!configs.empty() && configs.front().kind == PolicyKind::VanillaEC) {
            vanilla_p99 = results.front().aggregate.p99;
        } else {
            PolicyConfig vanilla_config{.kind=PolicyKind::VanillaEC};
            vanilla_p99 =
                run_dynamic_policy(world, layout, vanilla_config).aggregate.p99;
        }

        if (runtime.format == OutputFormat::Table) {
            print_table_header(runtime, health_params);
            for (std::size_t i = 0; i < configs.size(); ++i)
                print_table_row(configs[i].kind, results[i].aggregate, vanilla_p99);
        } else if (runtime.format == OutputFormat::Csv) {
            print_dynamic_csv_header();
            for (std::size_t i = 0; i < configs.size(); ++i)
                print_dynamic_csv_row(
                    runtime, world, configs[i].kind, results[i], vanilla_p99);
        } else if (runtime.format == OutputFormat::WindowedCsv) {
            print_windowed_csv_header();
            for (std::size_t i = 0; i < configs.size(); ++i)
                print_windowed_csv_rows(runtime, world, configs[i].kind, results[i]);
        }

        std::filesystem::remove_all(TMP);
        return 0;
    }

    std::vector<PolicyConfig> configs;
    std::vector<RunResult> results;
    configs.reserve(policies.size());
    results.reserve(policies.size());

    for (PolicyKind kind : policies) {
        configs.push_back(make_policy_config(kind, runtime, health_params));
        results.push_back(run_one_policy(runtime, layout, configs.back()));
    }

    double vanilla_p99 = 0.0;
    if (!configs.empty() && configs.front().kind == PolicyKind::VanillaEC) {
        vanilla_p99 = results.front().p99;
    } else {
        PolicyConfig vanilla_config{.kind=PolicyKind::VanillaEC};
        vanilla_p99 = run_one_policy(runtime, layout, vanilla_config).p99;
    }

    if (runtime.format == OutputFormat::Table) {
        print_table_header(runtime, health_params);
    } else {
        print_static_csv_header();
    }

    for (std::size_t i = 0; i < configs.size(); ++i) {
        if (runtime.format == OutputFormat::Table) {
            print_table_row(configs[i].kind, results[i], vanilla_p99);
        } else {
            print_static_csv_row(runtime, configs[i].kind, results[i], vanilla_p99);
        }
    }

    std::filesystem::remove_all(TMP);
    return 0;
}
