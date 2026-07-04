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
#include <deque>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <unistd.h>

using namespace healthec::core;
using namespace healthec::sim;

// Canonical T2.3/T2.4 workload.

static constexpr int      K                   = 4;
static constexpr int      M                   = 2;
static constexpr int      DEFAULT_NUM_DISKS   = 10;
static constexpr int      REALISTIC_NUM_DISKS = 100;
static constexpr int      DEFAULT_NUM_STRIPES = 500;
static constexpr int      DEFAULT_NUM_READS   = 20000;
static constexpr uint64_t DEFAULT_SEED        = 42;
static constexpr double   DEFAULT_ZIPF_S      = 1.0;
static constexpr double   DEFAULT_TIMEOUT_MS  = 30.0;
static constexpr double   DYNAMIC_DEFAULT_TIMEOUT_MS = 15.0;

static constexpr int DEFAULT_SLOW_DISK_A = 8;
static constexpr int DEFAULT_SLOW_DISK_B = 9;
static constexpr int REALISTIC_SLOW_DISK_A = 98;
static constexpr int REALISTIC_SLOW_DISK_B = 99;
static constexpr int STRESS100_SLOW_DISK_A = 80;
static constexpr int STRESS100_SLOW_DISK_B = 90;
static constexpr int OVERLAP_SLOW_DISK_A = 96;
static constexpr int OVERLAP_SLOW_DISK_B = 99;

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

struct ScenarioSpec {
    const char* name;
    bool is_dynamic;
    int num_disks;
    DiskId slow_disk_a;
    DiskId slow_disk_b;
    std::vector<DiskId> slow_disks;
    std::vector<DiskId> slow_disk_group_a;
    std::vector<DiskId> slow_disk_group_b;
    bool stripe0_special_case;
    bool precompute_all_disk_latencies;
};

static std::vector<DiskId> disk_range(DiskId first, DiskId last_inclusive) {
    std::vector<DiskId> out;
    for (DiskId d = first; d <= last_inclusive; ++d)
        out.push_back(d);
    return out;
}

static ScenarioSpec make_dynamic_sensitivity_spec(
    const char* name,
    DiskId group_a_first,
    DiskId group_a_last,
    DiskId group_b_first,
    DiskId group_b_last)
{
    std::vector<DiskId> group_a = disk_range(group_a_first, group_a_last);
    std::vector<DiskId> group_b = disk_range(group_b_first, group_b_last);
    std::vector<DiskId> all = group_a;
    all.insert(all.end(), group_b.begin(), group_b.end());
    return {
        name,
        true,
        REALISTIC_NUM_DISKS,
        group_a.front(),
        group_b.front(),
        all,
        group_a,
        group_b,
        false,
        false,
    };
}

static ScenarioSpec scenario_spec_for_name(const std::string& scenario) {
    if (scenario == "canonical_stress20") {
        return {
            "canonical_stress20",
            false,
            DEFAULT_NUM_DISKS,
            DEFAULT_SLOW_DISK_A,
            DEFAULT_SLOW_DISK_B,
            {DEFAULT_SLOW_DISK_A, DEFAULT_SLOW_DISK_B},
            {DEFAULT_SLOW_DISK_A},
            {DEFAULT_SLOW_DISK_B},
            true,
            false,
        };
    }
    if (scenario == "dynamic_degradation") {
        return {
            "dynamic_degradation",
            true,
            DEFAULT_NUM_DISKS,
            DEFAULT_SLOW_DISK_A,
            DEFAULT_SLOW_DISK_B,
            {DEFAULT_SLOW_DISK_A, DEFAULT_SLOW_DISK_B},
            {DEFAULT_SLOW_DISK_A},
            {DEFAULT_SLOW_DISK_B},
            true,
            true,
        };
    }
    if (scenario == "dynamic_realistic_100d_2pct_hdd") {
        return {
            "dynamic_realistic_100d_2pct_hdd",
            true,
            REALISTIC_NUM_DISKS,
            REALISTIC_SLOW_DISK_A,
            REALISTIC_SLOW_DISK_B,
            {REALISTIC_SLOW_DISK_A, REALISTIC_SLOW_DISK_B},
            {REALISTIC_SLOW_DISK_A},
            {REALISTIC_SLOW_DISK_B},
            false,
            false,
        };
    }
    if (scenario == "dynamic_stress_100d_20pct_hdd") {
        std::vector<DiskId> group_a = disk_range(80, 89);
        std::vector<DiskId> group_b = disk_range(90, 99);
        std::vector<DiskId> all = group_a;
        all.insert(all.end(), group_b.begin(), group_b.end());
        return {
            "dynamic_stress_100d_20pct_hdd",
            true,
            REALISTIC_NUM_DISKS,
            STRESS100_SLOW_DISK_A,
            STRESS100_SLOW_DISK_B,
            all,
            group_a,
            group_b,
            false,
            false,
        };
    }
    if (scenario == "dynamic_overlap_100d_4pct_hdd") {
        std::vector<DiskId> all = disk_range(96, 99);
        return {
            "dynamic_overlap_100d_4pct_hdd",
            true,
            REALISTIC_NUM_DISKS,
            OVERLAP_SLOW_DISK_A,
            OVERLAP_SLOW_DISK_B,
            all,
            {},
            {},
            false,
            false,
        };
    }
    if (scenario == "dynamic_sensitivity_100d_2pct_hdd") {
        return make_dynamic_sensitivity_spec(
            "dynamic_sensitivity_100d_2pct_hdd", 98, 98, 99, 99);
    }
    if (scenario == "dynamic_sensitivity_100d_4pct_hdd") {
        return make_dynamic_sensitivity_spec(
            "dynamic_sensitivity_100d_4pct_hdd", 96, 97, 98, 99);
    }
    if (scenario == "dynamic_sensitivity_100d_10pct_hdd") {
        return make_dynamic_sensitivity_spec(
            "dynamic_sensitivity_100d_10pct_hdd", 90, 94, 95, 99);
    }
    if (scenario == "dynamic_sensitivity_100d_20pct_hdd") {
        return make_dynamic_sensitivity_spec(
            "dynamic_sensitivity_100d_20pct_hdd", 80, 89, 90, 99);
    }
    throw std::invalid_argument("invalid --scenario: " + scenario);
}

static bool is_valid_scenario_name(const std::string& scenario) {
    return scenario == "canonical_stress20" ||
           scenario == "dynamic_degradation" ||
           scenario == "dynamic_realistic_100d_2pct_hdd" ||
           scenario == "dynamic_stress_100d_20pct_hdd" ||
           scenario == "dynamic_overlap_100d_4pct_hdd" ||
           scenario == "dynamic_sensitivity_100d_2pct_hdd" ||
           scenario == "dynamic_sensitivity_100d_4pct_hdd" ||
           scenario == "dynamic_sensitivity_100d_10pct_hdd" ||
           scenario == "dynamic_sensitivity_100d_20pct_hdd";
}

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
    p.theta_S = 0.03;
    p.theta_D = 12.0;
    p.loser_sig_ratio = 0.0;
    p.loser_sig_abs_ms = 25.0;
    p.parity_win_abs_ms = 15.0;
    return p;
}

static void configure_slow_disks(DiskSimulator& sim, const ScenarioSpec& spec) {
    for (DiskId disk : spec.slow_disk_group_a)
        sim.set_profile(disk, PROFILE_MILD);
    for (DiskId disk : spec.slow_disk_group_b)
        sim.set_profile(disk, PROFILE_SEVERE);
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

static Layout build_layout(int num_stripes, const ScenarioSpec& spec) {
    Layout L;
    std::unordered_map<DiskId, bool> slow_disk_lookup;
    for (DiskId disk : spec.slow_disks)
        slow_disk_lookup[disk] = true;

    for (int s = 0; s < num_stripes; ++s) {
        StripeLayout sl;
        const int disk_map_s0[] = {
            6, 7, spec.slow_disk_a, spec.slow_disk_b, 4, 5};
        for (int i = 0; i < K + M; ++i) {
            ShardId sh = make_shard(s, i);
            DiskId dk = (spec.stripe0_special_case && s == 0)
                ? disk_map_s0[i]
                : (s * (K + M) + i) % spec.num_disks;
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
            if (slow_disk_lookup.count(dk))
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

enum class MigrationStrategyKind {
    Default,
    Disabled,
    ThresholdAlpha,
    WindowEvidence,
    TopKBudgeted,
    Hybrid,
};

enum class OutputFormat {
    Table,
    Csv,
    WindowedCsv,
    EventTrace,
    MigrationTrace,
};

struct MigrationStrategyConfig {
    MigrationStrategyKind kind = MigrationStrategyKind::Default;
    int evidence_windows = 0;
    int top_k_per_window = 0;
    int cooldown_windows = 0;
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
    double slowdown_scale = 1.0;
    ScoreParams health_ec_params = locked_health_ec_params();
    MigrationStrategyConfig migration_strategy;
    OutputFormat format = OutputFormat::Table;
    bool extended_metrics = false;
};

struct PolicyConfig {
    PolicyKind kind;
    double timeout_ms = 0.0;
    ScoreParams health_ec_params{};
    MigrationStrategyConfig migration_strategy{};
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

static MigrationStrategyKind parse_migration_strategy_kind(
    const std::string& value)
{
    if (value == "default") return MigrationStrategyKind::Default;
    if (value == "disabled") return MigrationStrategyKind::Disabled;
    if (value == "threshold_alpha") return MigrationStrategyKind::ThresholdAlpha;
    if (value == "window_evidence") return MigrationStrategyKind::WindowEvidence;
    if (value == "topk_budgeted") return MigrationStrategyKind::TopKBudgeted;
    if (value == "hybrid") return MigrationStrategyKind::Hybrid;
    throw std::invalid_argument(
        "invalid --health-migration-strategy: " + value);
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

static int parse_nonnegative_int(const std::string& value,
                                 const std::string& name) {
    std::size_t idx = 0;
    long parsed = std::stol(value, &idx, 10);
    if (idx != value.size() || parsed < 0 ||
        parsed > static_cast<long>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument(name + " must be a non-negative integer");
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
            if (!is_valid_scenario_name(cfg.scenario)) {
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
        } else if (arg == "--slowdown-scale") {
            cfg.slowdown_scale = parse_positive_double(require_value(arg), arg);
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
        } else if (arg == "--health-migration-strategy") {
            cfg.migration_strategy.kind =
                parse_migration_strategy_kind(require_value(arg));
        } else if (arg == "--health-alpha-d") {
            cfg.health_ec_params.alpha_D =
                parse_positive_double(require_value(arg), arg);
        } else if (arg == "--health-migration-evidence-windows") {
            cfg.migration_strategy.evidence_windows =
                parse_nonnegative_int(require_value(arg), arg);
        } else if (arg == "--health-migration-top-k-per-window") {
            cfg.migration_strategy.top_k_per_window =
                parse_nonnegative_int(require_value(arg), arg);
        } else if (arg == "--health-migration-cooldown-windows") {
            cfg.migration_strategy.cooldown_windows =
                parse_nonnegative_int(require_value(arg), arg);
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
            } else if (value == "migration_trace") {
                cfg.format = OutputFormat::MigrationTrace;
            } else {
                throw std::invalid_argument("invalid --format: " + value);
            }
        } else if (arg == "--extended-metrics") {
            cfg.extended_metrics = true;
        } else {
            throw std::invalid_argument("unknown argument: " + arg);
        }
    }
    return cfg;
}

static bool is_dynamic_scenario(const RuntimeConfig& cfg) {
    return scenario_spec_for_name(cfg.scenario).is_dynamic;
}

static void apply_runtime_defaults(RuntimeConfig& cfg) {
    if (is_dynamic_scenario(cfg) && !cfg.timeout_ms_explicit)
        cfg.timeout_ms = DYNAMIC_DEFAULT_TIMEOUT_MS;
}

static void validate_runtime_config(const RuntimeConfig& cfg) {
    if (!is_dynamic_scenario(cfg) &&
        (cfg.format == OutputFormat::WindowedCsv ||
         cfg.format == OutputFormat::EventTrace ||
         cfg.format == OutputFormat::MigrationTrace)) {
        throw std::invalid_argument(
            "windowed_csv, event_trace, and migration_trace require a dynamic scenario");
    }
    if (is_dynamic_scenario(cfg) && cfg.num_reads % DYNAMIC_NUM_WINDOWS != 0) {
        throw std::invalid_argument(
            "dynamic scenarios require --num-reads divisible by 20");
    }
    if (cfg.extended_metrics &&
        (!is_dynamic_scenario(cfg) || cfg.format != OutputFormat::Csv)) {
        throw std::invalid_argument(
            "extended metrics require a dynamic scenario with --format csv");
    }
    if (cfg.format == OutputFormat::MigrationTrace &&
        (cfg.policy_selection != PolicySelection::Single ||
         cfg.single_policy != PolicyKind::HealthEC)) {
        throw std::invalid_argument(
            "migration_trace requires --policy health_ec");
    }
    if (cfg.health_ec_params.alpha_D >= cfg.health_ec_params.alpha_S) {
        throw std::invalid_argument(
            "--health-alpha-d must be less than alpha_S");
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
    if (kind == PolicyKind::HealthEC) {
        cfg.health_ec_params = health_params;
        cfg.migration_strategy = runtime.migration_strategy;
    }
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

                if (st.D > p.theta_D &&
                    config.migration_strategy.kind !=
                        MigrationStrategyKind::Disabled) {
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
    const ScenarioSpec spec = scenario_spec_for_name(runtime.scenario);
    const std::string TMP = runtime_tmp_dir("healthec_compare_policies", runtime.seed);
    DiskSimulator sim(TMP, spec.num_disks, PROFILE_BASELINE, runtime.seed);
    configure_slow_disks(sim, spec);
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
    int read_index;
    int window_id;
    DiskState state;
    double latency_ms;
    std::vector<double> latency_by_disk;
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
    int first_onset_window = FIRST_DYNAMIC_ONSET_WINDOW;
    int num_disks = DEFAULT_NUM_DISKS;
    DiskId slow_disk_a = DEFAULT_SLOW_DISK_A;
    DiskId slow_disk_b = DEFAULT_SLOW_DISK_B;
    uint64_t seed = DEFAULT_SEED;
    double slowdown_scale = 1.0;
    bool precompute_all_disk_latencies = true;
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

struct MigrationTraceRow {
    int read_index = 0;
    int window_id = 0;
    StripeId stripe_id = 0;
    ShardId shard_id = 0;
    DiskId source_disk = 0;
    DiskId target_disk = 0;
    bool is_migration_positive = false;
    int event_id = -1;
    DiskState disk_state = DiskState::Healthy;
};

struct DynamicRunResult {
    RunResult aggregate;
    std::vector<double> aggregate_latencies;
    std::vector<DynamicWindowResult> windows;
    double post_onset_p95_auc_ms = 0.0;
    double post_onset_p99_auc_ms = 0.0;
    double post_warmup_windowed_p99_ms = NOT_APPLICABLE;
    double severe_window_p99_ms = NOT_APPLICABLE;
    long pre_slowdown_parity_reads = 0;
    long recovery_parity_reads = 0;
    long migration_true_positives = 0;
    long migration_false_negatives = NOT_APPLICABLE;
    long first_detection_latency_reads = NOT_APPLICABLE;
    long first_mitigation_latency_reads = NOT_APPLICABLE;
    long recovery_regret_reads = 0;
    long read_bypass_eligible_events = NOT_APPLICABLE;
    long read_bypass_triggered_events = NOT_APPLICABLE;
    std::vector<MigrationTraceRow> migration_trace;
};

struct EventShardStats {
    int read_count = 0;
    bool migrated = false;
    bool trigger_recorded = false;
    int trigger_window = -1;
};

struct ScoreState {
    double S = 0.0;
    double D = 0.0;
};

struct EvidenceState {
    int last_window = -1;
    int consecutive_windows = 0;
};

struct PendingMigrationCandidate {
    ShardId shard_id = 0;
    LatentRequest request;
    double max_death_score = 0.0;
};

static bool is_migration_positive(DiskState state) {
    return state == DiskState::MildSlow || state == DiskState::SevereSlow;
}

static bool is_severe_state(DiskState state) {
    return state == DiskState::SevereSlow;
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

static DiskProfile scale_slow_profile(const DiskProfile& profile, double scale) {
    DiskProfile scaled = profile;
    scaled.slow_mean_ms =
        PROFILE_BASELINE.base_mean_ms +
        scale * (profile.slow_mean_ms - PROFILE_BASELINE.base_mean_ms);
    scaled.slow_jitter_ms =
        PROFILE_BASELINE.base_jitter_ms +
        scale * (profile.slow_jitter_ms - PROFILE_BASELINE.base_jitter_ms);
    scaled.spike_prob = std::clamp(scale * profile.spike_prob, 0.0, 1.0);
    scaled.spike_ms = scale * profile.spike_ms;
    return scaled;
}

static DiskProfile profile_for_state(DiskState state, double slowdown_scale) {
    switch (state) {
        case DiskState::Healthy: return PROFILE_BASELINE;
        case DiskState::MildSlow:
            return scale_slow_profile(PROFILE_MILD, slowdown_scale);
        case DiskState::SevereSlow:
            return scale_slow_profile(PROFILE_SEVERE, slowdown_scale);
        case DiskState::Recovery:
            return scale_slow_profile(PROFILE_RECOVERY, slowdown_scale);
    }
    return PROFILE_BASELINE;
}

static std::vector<ScheduleEvent> dynamic_schedule(const ScenarioSpec& spec) {
    std::vector<ScheduleEvent> schedule;
    int event_id = 0;
    auto add_event = [&](DiskId disk, DiskState state,
                         int start_window, int end_window,
                         const char* notes) {
        schedule.push_back({
            event_id++, disk, state, start_window, end_window, notes});
    };

    if (std::string(spec.name) == "dynamic_overlap_100d_4pct_hdd") {
        add_event(96, DiskState::Healthy,    0,  3, "first_disk_warmup");
        add_event(96, DiskState::MildSlow,   3,  6, "first_disk_gradual_degradation");
        add_event(96, DiskState::SevereSlow, 6, 10, "first_disk_sustained_severe_period");
        add_event(96, DiskState::Recovery,  10, 12, "first_disk_partial_recovery");
        add_event(96, DiskState::Healthy,   12, 20, "first_disk_post_recovery_observation");

        add_event(97, DiskState::Healthy,    0,  5, "second_disk_staggered_warmup");
        add_event(97, DiskState::MildSlow,   5,  8, "second_disk_gradual_degradation");
        add_event(97, DiskState::SevereSlow, 8, 12, "second_disk_sustained_severe_period");
        add_event(97, DiskState::Recovery,  12, 14, "second_disk_partial_recovery");
        add_event(97, DiskState::Healthy,   14, 20, "second_disk_post_recovery_observation");

        add_event(98, DiskState::Healthy,    0,  8, "third_disk_late_warmup");
        add_event(98, DiskState::MildSlow,   8, 11, "third_disk_gradual_degradation");
        add_event(98, DiskState::SevereSlow, 11, 15, "third_disk_sustained_severe_period");
        add_event(98, DiskState::Recovery,  15, 17, "third_disk_partial_recovery");
        add_event(98, DiskState::Healthy,   17, 20, "third_disk_post_recovery_observation");

        add_event(99, DiskState::Healthy,    0, 11, "fourth_disk_latest_warmup");
        add_event(99, DiskState::MildSlow,   11, 14, "fourth_disk_gradual_degradation");
        add_event(99, DiskState::SevereSlow, 14, 17, "fourth_disk_sustained_severe_period");
        add_event(99, DiskState::Recovery,   17, 18, "fourth_disk_short_recovery");
        add_event(99, DiskState::Healthy,    18, 20, "fourth_disk_post_recovery_observation");
        return schedule;
    }

    auto add_group_a = [&](DiskId disk) {
        schedule.push_back({event_id++, disk, DiskState::Healthy,    0,  3, "warmup_before_first_onset"});
        schedule.push_back({event_id++, disk, DiskState::MildSlow,   3,  6, "first_gradual_degradation"});
        schedule.push_back({event_id++, disk, DiskState::SevereSlow, 6,  9, "sustained_severe_period"});
        schedule.push_back({event_id++, disk, DiskState::Recovery,   9, 11, "partial_recovery"});
        schedule.push_back({event_id++, disk, DiskState::Healthy,   11, 14, "recovered_interval"});
        schedule.push_back({event_id++, disk, DiskState::MildSlow,  14, 15, "relapse"});
        schedule.push_back({event_id++, disk, DiskState::Recovery,  15, 16, "relapse_recovery"});
        schedule.push_back({event_id++, disk, DiskState::Healthy,   16, 20, "post_recovery_observation"});
    };
    auto add_group_b = [&](DiskId disk) {
        schedule.push_back({event_id++, disk, DiskState::Healthy,    0,  9, "staggered_later_onset"});
        schedule.push_back({event_id++, disk, DiskState::MildSlow,   9, 11, "second_disk_mild_period"});
        schedule.push_back({event_id++, disk, DiskState::SevereSlow, 11, 14, "second_disk_severe_period"});
        schedule.push_back({event_id++, disk, DiskState::Recovery,  14, 16, "second_disk_recovery"});
        schedule.push_back({event_id++, disk, DiskState::Healthy,   16, 20, "second_disk_post_recovery"});
    };

    for (DiskId disk : spec.slow_disk_group_a)
        add_group_a(disk);
    for (DiskId disk : spec.slow_disk_group_b)
        add_group_b(disk);
    return schedule;
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

static double deterministic_latency_ms(uint64_t seed,
                                       int read_index,
                                       ShardId shard,
                                       DiskId disk,
                                       bool is_parity,
                                       const DiskProfile& profile);

static double latency_on_disk(const LatentWorld& world,
                              const LatentShard& shard,
                              DiskId disk) {
    if (disk < 0 || disk >= world.num_disks)
        throw std::out_of_range("latency_on_disk: disk out of range");
    if (!shard.latency_by_disk.empty())
        return shard.latency_by_disk.at(static_cast<std::size_t>(disk));

    DiskState disk_state = state_for_disk_window(
        world.schedule, disk, shard.window_id);
    return deterministic_latency_ms(
        world.seed, shard.read_index, shard.shard, disk,
        shard.is_parity, profile_for_state(disk_state, world.slowdown_scale));
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
    std::unordered_map<DiskId, bool> active;
    for (const auto& e : schedule) {
        if (window_id >= e.start_window &&
            window_id < e.end_window &&
            is_migration_positive(e.state)) {
            active[e.disk_id] = true;
        }
    }
    return static_cast<int>(active.size());
}

static bool any_severe_disk_for_window(
    const std::vector<ScheduleEvent>& schedule, int window_id)
{
    for (const auto& e : schedule) {
        if (window_id >= e.start_window &&
            window_id < e.end_window &&
            is_severe_state(e.state)) {
            return true;
        }
    }
    return false;
}

static uint64_t splitmix64_next(uint64_t& x) {
    uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static double uniform01(uint64_t& state) {
    constexpr double DENOM = 9007199254740992.0; // 2^53
    return static_cast<double>(splitmix64_next(state) >> 11) / DENOM;
}

static uint64_t deterministic_latency_seed(uint64_t seed,
                                           int read_index,
                                           ShardId shard,
                                           DiskId disk,
                                           bool is_parity)
{
    uint64_t state = seed ^ 0xD1B54A32D192ED03ULL;
    state ^= static_cast<uint64_t>(read_index) * 0x9E3779B97F4A7C15ULL;
    state ^= static_cast<uint64_t>(shard) * 0xBF58476D1CE4E5B9ULL;
    state ^= static_cast<uint64_t>(disk) * 0x94D049BB133111EBULL;
    if (is_parity)
        state ^= 0xA24BAED4963EE407ULL;
    return splitmix64_next(state);
}

static double deterministic_latency_ms(uint64_t seed,
                                       int read_index,
                                       ShardId shard,
                                       DiskId disk,
                                       bool is_parity,
                                       const DiskProfile& profile)
{
    uint64_t state = deterministic_latency_seed(
        seed, read_index, shard, disk, is_parity);
    double mean = profile.slow_mode ? profile.slow_mean_ms : profile.base_mean_ms;
    double jitter = profile.slow_mode ? profile.slow_jitter_ms
                                      : profile.base_jitter_ms;

    double lat = mean;
    if (jitter != 0.0) {
        constexpr double TWO_PI = 6.28318530717958647692;
        double u1 = std::max(uniform01(state), std::numeric_limits<double>::min());
        double u2 = uniform01(state);
        double z = std::sqrt(-2.0 * std::log(u1)) * std::cos(TWO_PI * u2);
        lat = mean + jitter * z;
    }
    if (profile.slow_mode && profile.spike_prob > 0.0 &&
        uniform01(state) < profile.spike_prob) {
        lat += profile.spike_ms;
    }
    return std::max(0.0, lat);
}

static LatentWorld build_dynamic_world(const RuntimeConfig& runtime,
                                       const Layout& layout)
{
    const ScenarioSpec spec = scenario_spec_for_name(runtime.scenario);
    LatentWorld world;
    world.window_size = runtime.num_reads / DYNAMIC_NUM_WINDOWS;
    world.first_onset_window = FIRST_DYNAMIC_ONSET_WINDOW;
    world.num_disks = spec.num_disks;
    world.slow_disk_a = spec.slow_disk_a;
    world.slow_disk_b = spec.slow_disk_b;
    world.seed = runtime.seed;
    world.slowdown_scale = runtime.slowdown_scale;
    world.precompute_all_disk_latencies = spec.precompute_all_disk_latencies;
    world.schedule = dynamic_schedule(spec);
    world.requests.reserve(runtime.num_reads);

    const std::string TMP = runtime_tmp_dir(
        "healthec_compare_policies_dynamic_trace", runtime.seed);
    std::unique_ptr<DiskSimulator> sim;
    if (world.precompute_all_disk_latencies) {
        std::filesystem::remove_all(TMP);
        sim = std::make_unique<DiskSimulator>(
            TMP, world.num_disks, PROFILE_BASELINE, runtime.seed);
    }
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

        auto make_latent = [&](ShardId sh, bool is_parity) {
            DiskId disk = sl.disk_of.at(sh);
            DiskState state = state_for_disk_window(world.schedule, disk, window_id);
            LatentShard latent{.shard=sh, .disk=disk, .is_parity=is_parity,
                                .read_index=r, .window_id=window_id,
                                .state=state, .latency_ms=0.0,
                                .latency_by_disk={}};
            if (world.precompute_all_disk_latencies) {
                latent.latency_by_disk.resize(
                    static_cast<std::size_t>(world.num_disks));
                for (DiskId d = 0; d < world.num_disks; ++d) {
                    DiskState disk_state = state_for_disk_window(
                        world.schedule, d, window_id);
                    sim->set_profile(
                        d, profile_for_state(disk_state, world.slowdown_scale));
                    latent.latency_by_disk[static_cast<std::size_t>(d)] =
                        sim->sample_latency_ms(d);
                }
            }
            latent.latency_ms = latency_on_disk(world, latent, disk);
            return latent;
        };

        for (ShardId sh : sl.data_shards) {
            req.data.push_back(make_latent(sh, false));
        }
        for (ShardId sh : sl.parity_shards) {
            req.parity.push_back(make_latent(sh, true));
        }

        world.requests.push_back(std::move(req));
    }

    if (world.precompute_all_disk_latencies)
        std::filesystem::remove_all(TMP);
    return world;
}

static std::vector<std::pair<ShardId, double>> latent_data_latencies(
    const LatentWorld& world,
    const LatentRequest& req,
    const std::unordered_map<ShardId, DiskId>& logical_disk)
{
    std::vector<std::pair<ShardId, double>> out;
    out.reserve(req.data.size());
    for (const auto& sh : req.data) {
        DiskId disk = logical_disk_for(logical_disk, sh);
        out.emplace_back(sh.shard, latency_on_disk(world, sh, disk));
    }
    return out;
}

static const LatentShard& select_parity_for_dynamic_read(
    const LatentRequest& req,
    const std::unordered_map<ShardId, DiskId>& logical_disk,
    const std::vector<double>& disk_health,
    bool health_aware)
{
    const LatentShard* best = &req.parity.front();
    if (!health_aware)
        return *best;

    double best_health = disk_health[logical_disk_for(logical_disk, *best)];
    for (std::size_t i = 1; i < req.parity.size(); ++i) {
        const LatentShard& candidate = req.parity[i];
        double health = disk_health[logical_disk_for(logical_disk, candidate)];
        if (health > best_health) {
            best = &candidate;
            best_health = health;
        }
    }
    return *best;
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

static bool read_bypass_eligible_with_parity(
    const LatentWorld& world,
    const LatentRequest& req,
    const std::unordered_map<ShardId, DiskId>& logical_disk,
    ShardId parity_shard,
    double parity_lat,
    const std::vector<std::pair<ShardId, double>>& data_lat)
{
    auto all = sorted_with_parity(data_lat, parity_shard, parity_lat);
    bool parity_in_k = false;
    std::unordered_map<ShardId, bool> in_k;
    for (int i = 0; i < K; ++i) {
        in_k[all[i].first] = true;
        if (all[i].first == parity_shard)
            parity_in_k = true;
    }
    return positive_data_bypassed_by_parity(
        world, req, logical_disk, in_k, parity_in_k);
}

static double coverage_pct(long triggered, long eligible) {
    if (eligible <= 0) return static_cast<double>(NOT_APPLICABLE);
    return static_cast<double>(triggered) * 100.0 / static_cast<double>(eligible);
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

    if (req.window_id < world.first_onset_window)
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

static void update_observed_health(std::vector<double>& disk_health,
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
    const std::vector<double>& disk_health,
    DiskId current_disk)
{
    std::vector<bool> used(disk_health.size(), false);
    for (const auto& sh : req.data)
        used[logical_disk_for(logical_disk, sh)] = true;
    for (const auto& sh : req.parity)
        used[sh.disk] = true;

    used[current_disk] = false;
    DiskId target = -1;
    double target_health = disk_health[current_disk];
    for (DiskId d = 0; d < static_cast<DiskId>(disk_health.size()); ++d) {
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
    DiskId target_disk,
    std::unordered_map<int, std::unordered_map<ShardId, EventShardStats>>& event_stats)
{
    result.aggregate.migration_triggers++;
    result.windows.at(req.window_id).migration_triggers++;

    const ScheduleEvent* e = event_for_disk_window(
        world.schedule, current_disk, req.window_id);
    DiskState state = e ? e->state : DiskState::Healthy;
    int event_key = e ? e->event_id : (-1000 - current_disk);
    result.migration_trace.push_back({
        .read_index = req.read_index,
        .window_id = req.window_id,
        .stripe_id = req.stripe_id,
        .shard_id = shard.shard,
        .source_disk = current_disk,
        .target_disk = target_disk,
        .is_migration_positive = is_migration_positive(state),
        .event_id = event_key,
        .disk_state = state,
    });
    auto& stats = event_stats[event_key][shard.shard];

    if (!stats.trigger_recorded) {
        stats.trigger_recorded = true;
        if (is_migration_positive(state)) {
            stats.trigger_window = req.window_id;
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

static bool migration_strategy_uses_topk(const MigrationStrategyConfig& config) {
    return config.kind == MigrationStrategyKind::TopKBudgeted ||
           config.kind == MigrationStrategyKind::Hybrid;
}

static bool migration_strategy_uses_evidence(
    const MigrationStrategyConfig& config)
{
    return config.kind == MigrationStrategyKind::WindowEvidence ||
           config.kind == MigrationStrategyKind::Hybrid;
}

static void record_migration_evidence(
    std::unordered_map<ShardId, EvidenceState>& evidence,
    ShardId shard,
    int window_id)
{
    auto& state = evidence[shard];
    if (state.last_window == window_id)
        return;
    if (state.last_window == window_id - 1)
        state.consecutive_windows++;
    else
        state.consecutive_windows = 1;
    state.last_window = window_id;
}

static bool evidence_gate_passes(
    const MigrationStrategyConfig& config,
    const std::unordered_map<ShardId, EvidenceState>& evidence,
    ShardId shard)
{
    if (!migration_strategy_uses_evidence(config) ||
        config.evidence_windows <= 0) {
        return true;
    }
    auto it = evidence.find(shard);
    return it != evidence.end() &&
           it->second.consecutive_windows >= config.evidence_windows;
}

static bool cooldown_gate_passes(
    const MigrationStrategyConfig& config,
    const std::unordered_map<ShardId, int>& cooldown_until_window,
    ShardId shard,
    int window_id)
{
    if (config.cooldown_windows <= 0)
        return true;
    auto it = cooldown_until_window.find(shard);
    return it == cooldown_until_window.end() || window_id > it->second;
}

static bool try_apply_migration(
    DynamicRunResult& result,
    const LatentWorld& world,
    const LatentRequest& req,
    ShardId shard,
    std::unordered_map<ShardId, DiskId>& logical_disk,
    const std::vector<double>& disk_health,
    std::unordered_map<int, std::unordered_map<ShardId, EventShardStats>>& event_stats,
    std::unordered_map<ShardId, ScoreState>& scores,
    std::unordered_map<ShardId, int>& cooldown_until_window,
    const MigrationStrategyConfig& strategy)
{
    const LatentShard* data_shard = find_data_shard(req, shard);
    if (!data_shard)
        return false;
    DiskId current_disk = logical_disk_for(logical_disk, *data_shard);
    DiskId target = select_migration_target(
        req, logical_disk, disk_health, current_disk);
    if (target == -1)
        return false;

    record_migration_trigger(
        result, world, req, *data_shard, current_disk, target, event_stats);
    logical_disk[shard] = target;
    scores[shard].S = 0.0;
    scores[shard].D = 0.0;
    if (strategy.cooldown_windows > 0) {
        cooldown_until_window[shard] =
            req.window_id + strategy.cooldown_windows;
    }
    return true;
}

static void queue_migration_candidate(
    std::unordered_map<ShardId, PendingMigrationCandidate>& queued,
    const LatentRequest& req,
    ShardId shard,
    double death_score)
{
    auto& candidate = queued[shard];
    if (candidate.shard_id == 0 && shard != 0)
        candidate.shard_id = shard;
    if (candidate.request.data.empty() || death_score > candidate.max_death_score) {
        candidate.shard_id = shard;
        candidate.request = req;
        candidate.max_death_score = death_score;
    }
}

static void apply_queued_migrations(
    DynamicRunResult& result,
    const LatentWorld& world,
    const LatentRequest& current_req,
    std::unordered_map<ShardId, PendingMigrationCandidate>& queued,
    std::unordered_map<ShardId, DiskId>& logical_disk,
    const std::vector<double>& disk_health,
    std::unordered_map<int, std::unordered_map<ShardId, EventShardStats>>& event_stats,
    std::unordered_map<ShardId, ScoreState>& scores,
    std::unordered_map<ShardId, int>& cooldown_until_window,
    const MigrationStrategyConfig& strategy)
{
    if (queued.empty() || strategy.top_k_per_window <= 0) {
        queued.clear();
        return;
    }
    std::vector<PendingMigrationCandidate> candidates;
    candidates.reserve(queued.size());
    for (const auto& [shard, candidate] : queued) {
        (void)shard;
        candidates.push_back(candidate);
    }
    std::sort(
        candidates.begin(), candidates.end(),
        [](const auto& a, const auto& b) {
            if (a.max_death_score != b.max_death_score)
                return a.max_death_score > b.max_death_score;
            return a.shard_id < b.shard_id;
        });

    int applied = 0;
    for (auto candidate : candidates) {
        if (applied >= strategy.top_k_per_window)
            break;
        if (!cooldown_gate_passes(
                strategy, cooldown_until_window, candidate.shard_id,
                current_req.window_id)) {
            continue;
        }
        candidate.request.read_index = current_req.read_index;
        candidate.request.window_id = current_req.window_id;
        if (try_apply_migration(
                result, world, candidate.request, candidate.shard_id,
                logical_disk, disk_health, event_stats, scores,
                cooldown_until_window, strategy)) {
            applied++;
        }
    }
    queued.clear();
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

static void finalize_migration_truth(
    DynamicRunResult& result,
    const std::unordered_map<int, std::unordered_map<ShardId, EventShardStats>>& event_stats)
{
    result.migration_true_positives = 0;
    result.migration_false_negatives = 0;

    for (const auto& [event_id, shards] : event_stats) {
        (void)event_id;
        for (const auto& [shard, stats] : shards) {
            (void)shard;
            if (stats.read_count < 5)
                continue;

            if (stats.migrated) {
                result.migration_true_positives++;
                if (stats.trigger_window >= 0)
                    result.windows.at(stats.trigger_window).migration_true_positives++;
            } else {
                result.migration_false_negatives++;
            }
        }
    }
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
    for (int w = 0; w < world.first_onset_window; ++w) {
        warmup_p95.push_back(result.windows.at(w).p95);
        warmup_p99.push_back(result.windows.at(w).p99);
    }
    double baseline_p95 = percentile_copy(warmup_p95, 0.50);
    double baseline_p99 = percentile_copy(warmup_p99, 0.50);

    for (int w = world.first_onset_window; w < world.num_windows; ++w) {
        result.post_onset_p95_auc_ms +=
            std::max(result.windows.at(w).p95 - baseline_p95, 0.0);
        result.post_onset_p99_auc_ms +=
            std::max(result.windows.at(w).p99 - baseline_p99, 0.0);
    }

    std::vector<double> post_warmup_p99;
    std::vector<double> severe_p99;
    for (int w = world.first_onset_window; w < world.num_windows; ++w) {
        post_warmup_p99.push_back(result.windows.at(w).p99);
        if (any_severe_disk_for_window(world.schedule, w)) {
            severe_p99.push_back(result.windows.at(w).p99);
        }
    }
    result.post_warmup_windowed_p99_ms =
        post_warmup_p99.empty() ? static_cast<double>(NOT_APPLICABLE)
                                : percentile_copy(post_warmup_p99, 0.50);
    result.severe_window_p99_ms =
        severe_p99.empty() ? static_cast<double>(NOT_APPLICABLE)
                           : percentile_copy(severe_p99, 0.50);
}

static DynamicRunResult run_dynamic_policy(const LatentWorld& world,
                                           const Layout& layout,
                                           const PolicyConfig& config)
{
    std::unordered_map<ShardId, ScoreState> scores;
    std::unordered_map<ShardId, DiskId> logical_disk = initial_logical_disks(layout);
    std::vector<double> disk_health(
        static_cast<std::size_t>(world.num_disks), 1.0);
    std::unordered_map<int, std::unordered_map<ShardId, EventShardStats>> event_stats;
    std::unordered_map<ShardId, EvidenceState> migration_evidence;
    std::unordered_map<ShardId, int> cooldown_until_window;
    std::unordered_map<ShardId, PendingMigrationCandidate> queued_migrations;

    DynamicRunResult result;
    result.windows = make_dynamic_windows(world);
    result.aggregate_latencies.reserve(world.requests.size());
    if (config.kind != PolicyKind::HealthEC) {
        result.migration_true_positives = NOT_APPLICABLE;
        result.aggregate.migration_false_positives = NOT_APPLICABLE;
        result.migration_false_negatives = NOT_APPLICABLE;
    } else {
        result.read_bypass_eligible_events = 0;
        result.read_bypass_triggered_events = 0;
    }

    const int first_onset_read = world.first_onset_window * world.window_size;
    int current_window = -1;

    for (const auto& req : world.requests) {
        if (config.kind == PolicyKind::HealthEC &&
            migration_strategy_uses_topk(config.migration_strategy) &&
            req.window_id != current_window) {
            if (current_window >= 0) {
                apply_queued_migrations(
                    result, world, req, queued_migrations, logical_disk,
                    disk_health, event_stats, scores, cooldown_until_window,
                    config.migration_strategy);
            }
            current_window = req.window_id;
        } else if (req.window_id != current_window) {
            current_window = req.window_id;
        }

        record_data_reads(result, req);
        if (config.kind == PolicyKind::HealthEC)
            record_positive_event_reads(world, req, logical_disk, event_stats);

        auto data_lat = latent_data_latencies(world, req, logical_disk);
        const LatentShard& parity = select_parity_for_dynamic_read(
            req, logical_disk, disk_health, config.kind == PolicyKind::HealthEC);
        ShardId parity_shard = parity.shard;
        DiskId parity_disk = logical_disk_for(logical_disk, parity);
        double parity_lat = latency_on_disk(world, parity, parity_disk);
        bool read_bypass_eligible = false;

        if (config.kind == PolicyKind::HealthEC) {
            for (const auto& sh : req.data) {
                DiskId disk = logical_disk_for(logical_disk, sh);
                update_observed_health(disk_health, disk,
                                       latency_on_disk(world, sh, disk),
                                       config.health_ec_params);
            }
            read_bypass_eligible = read_bypass_eligible_with_parity(
                world, req, logical_disk, parity_shard, parity_lat, data_lat);
            if (read_bypass_eligible)
                result.read_bypass_eligible_events++;
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
        if (read_bypass_eligible)
            result.read_bypass_triggered_events++;
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

            if (win > 0.0)
                record_migration_evidence(migration_evidence, sh, req.window_id);

            if (st.D > p.theta_D &&
                config.migration_strategy.kind !=
                    MigrationStrategyKind::Disabled &&
                evidence_gate_passes(
                    config.migration_strategy, migration_evidence, sh) &&
                cooldown_gate_passes(
                    config.migration_strategy, cooldown_until_window, sh,
                    req.window_id)) {
                if (migration_strategy_uses_topk(config.migration_strategy)) {
                    queue_migration_candidate(
                        queued_migrations, req, sh, st.D);
                } else {
                    try_apply_migration(
                        result, world, req, sh, logical_disk, disk_health,
                        event_stats, scores, cooldown_until_window,
                        config.migration_strategy);
                }
            }
        }
    }

    if (config.kind == PolicyKind::HealthEC)
        finalize_migration_truth(result, event_stats);

    finalize_dynamic_result(result, world);
    return result;
}

// Output.

static void print_table_header(const RuntimeConfig& runtime,
                               const ScoreParams& health_params)
{
    const ScenarioSpec spec = scenario_spec_for_name(runtime.scenario);
    std::cout << "compare_policies"
              << "  scenario=" << runtime.scenario
              << "  seed=" << runtime.seed
              << "  reads=" << runtime.num_reads
              << "  stripes=" << runtime.num_stripes
              << "  zipf_s=" << runtime.zipf_s << "\n";
    if (is_dynamic_scenario(runtime)) {
        std::cout << "Dynamic schedule: 20 windows; disk" << spec.slow_disk_a
                  << " first onset; disk" << spec.slow_disk_b
                  << " staggered onset; recovery/relapse included\n";
    } else {
        std::cout << "Slow disks: disk" << spec.slow_disk_a << "=mild  disk"
                  << spec.slow_disk_b << "=severe  ratio=20% stress\n";
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

static void print_dynamic_csv_header(const RuntimeConfig& runtime)
{
    std::cout
        << "scenario,num_disks,seed,num_reads,num_stripes,zipf_s,num_windows,window_size,"
        << "policy,timeout_ms,p50_ms,p95_ms,p99_ms,p99_improvement_pct,"
        << "post_onset_p95_auc_ms,post_onset_p99_auc_ms,issued_shard_reads,"
        << "bandwidth_overhead_pct,parity_reads,pre_slowdown_parity_reads,"
        << "recovery_parity_reads,proactive_or_degraded_reads,decode_count,"
        << "migration_triggers,migration_true_positives,migration_false_positives,"
        << "migration_false_negatives,first_detection_latency_reads,"
        << "first_mitigation_latency_reads,recovery_regret_reads";
    if (runtime.extended_metrics) {
        std::cout
            << ",post_warmup_windowed_p99_ms,severe_window_p99_ms,"
            << "read_bypass_eligible_events,read_bypass_triggered_events,"
            << "read_bypass_coverage_pct";
    }
    std::cout << "\n";
}

static void print_dynamic_csv_row(const RuntimeConfig& runtime,
                                  const LatentWorld& world,
                                  PolicyKind kind,
                                  const DynamicRunResult& r,
                                  double vanilla_p99)
{
    std::cout << std::fixed << std::setprecision(1)
              << runtime.scenario << ','
              << world.num_disks << ','
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
              << r.recovery_regret_reads;
    if (runtime.extended_metrics) {
        std::cout << ','
                  << r.post_warmup_windowed_p99_ms << ','
                  << r.severe_window_p99_ms << ','
                  << r.read_bypass_eligible_events << ','
                  << r.read_bypass_triggered_events << ','
                  << coverage_pct(r.read_bypass_triggered_events,
                                  r.read_bypass_eligible_events);
    }
    std::cout << "\n";
}

static void print_windowed_csv_header()
{
    std::cout
        << "scenario,num_disks,seed,num_reads,num_stripes,zipf_s,policy,timeout_ms,"
        << "window_id,window_start_read,window_end_read,"
        << "slow_disk_a_id,slow_disk_a_state,slow_disk_b_id,slow_disk_b_state,"
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
                  << world.num_disks << ','
                  << runtime.seed << ','
                  << runtime.num_reads << ','
                  << runtime.num_stripes << ','
                  << runtime.zipf_s << ','
                  << policy_name(kind) << ','
                  << runtime.timeout_ms << ','
                  << win.window_id << ','
                  << win.window_start_read << ','
                  << win.window_end_read << ','
                  << world.slow_disk_a << ','
                  << disk_state_name(state_for_disk_window(
                         world.schedule, world.slow_disk_a, win.window_id)) << ','
                  << world.slow_disk_b << ','
                  << disk_state_name(state_for_disk_window(
                         world.schedule, world.slow_disk_b, win.window_id)) << ','
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
    const ScenarioSpec spec = scenario_spec_for_name(runtime.scenario);
    int window_size = runtime.num_reads / DYNAMIC_NUM_WINDOWS;
    auto schedule = dynamic_schedule(spec);
    std::cout
        << "scenario,num_disks,seed,event_id,disk_id,state,start_window,end_window,"
        << "start_read,end_read,is_migration_positive,notes\n";
    for (const auto& e : schedule) {
        std::cout << runtime.scenario << ','
                  << spec.num_disks << ','
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

static void print_migration_trace_header()
{
    std::cout
        << "scenario,num_disks,seed,num_reads,num_stripes,zipf_s,policy,timeout_ms,"
        << "read_index,window_id,stripe_id,shard_id,source_disk,target_disk,"
        << "is_migration_positive,event_id,disk_state\n";
}

static void print_migration_trace_rows(const RuntimeConfig& runtime,
                                       const LatentWorld& world,
                                       const DynamicRunResult& result)
{
    std::cout << std::fixed << std::setprecision(1);
    for (const auto& row : result.migration_trace) {
        std::cout << runtime.scenario << ','
                  << world.num_disks << ','
                  << runtime.seed << ','
                  << runtime.num_reads << ','
                  << runtime.num_stripes << ','
                  << runtime.zipf_s << ','
                  << policy_name(PolicyKind::HealthEC) << ','
                  << runtime.timeout_ms << ','
                  << row.read_index << ','
                  << row.window_id << ','
                  << row.stripe_id << ','
                  << row.shard_id << ','
                  << row.source_disk << ','
                  << row.target_disk << ','
                  << (row.is_migration_positive ? 1 : 0) << ','
                  << row.event_id << ','
                  << disk_state_name(row.disk_state) << "\n";
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

    const ScenarioSpec spec = scenario_spec_for_name(runtime.scenario);
    const Layout layout = build_layout(runtime.num_stripes, spec);

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
            print_dynamic_csv_header(runtime);
            for (std::size_t i = 0; i < configs.size(); ++i)
                print_dynamic_csv_row(
                    runtime, world, configs[i].kind, results[i], vanilla_p99);
        } else if (runtime.format == OutputFormat::WindowedCsv) {
            print_windowed_csv_header();
            for (std::size_t i = 0; i < configs.size(); ++i)
                print_windowed_csv_rows(runtime, world, configs[i].kind, results[i]);
        } else if (runtime.format == OutputFormat::MigrationTrace) {
            print_migration_trace_header();
            print_migration_trace_rows(runtime, world, results.front());
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
