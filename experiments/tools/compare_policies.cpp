// compare_policies — T2.3 baseline comparison runner
//
// In-memory simulation over the locked T2.2 workload:
//   Vanilla EC / EC-Cache-style late binding / timeout degraded read / Health-EC.
//
// This intentionally stays in experiments/tools and does not call the async
// ReadScheduler or MigrationScheduler. It reuses the shared pure helpers from
// read_scheduler.h so experimental and production score gates do not drift.

#include "core/read_scheduler.h"
#include "core/score_manager.h"
#include "sim/disk_simulator.h"
#include "sim/workload_generator.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace healthec::core;
using namespace healthec::sim;

// ── Canonical T2.3 smoke workload ────────────────────────────────────────────

static constexpr int      K                   = 4;
static constexpr int      M                   = 2;
static constexpr int      NUM_DISKS           = 10;
static constexpr int      DEFAULT_NUM_STRIPES = 500;
static constexpr int      DEFAULT_NUM_READS   = 20000;
static constexpr uint64_t DEFAULT_SEED        = 42;
static constexpr double   DEFAULT_ZIPF_S      = 1.0;
static constexpr double   DEFAULT_TIMEOUT_MS  = 30.0;

static constexpr int MILD_DISK   = 8;
static constexpr int SEVERE_DISK = 9;

static const DiskProfile PROFILE_BASELINE{
    .base_mean_ms=9.0, .base_jitter_ms=3.5, .slow_mode=false};

static const DiskProfile PROFILE_MILD{
    .base_mean_ms=9.0, .base_jitter_ms=3.5, .slow_mode=true,
    .slow_mean_ms=24.0, .slow_jitter_ms=8.0, .spike_prob=0.01, .spike_ms=150.0};

static const DiskProfile PROFILE_SEVERE{
    .base_mean_ms=9.0, .base_jitter_ms=3.5, .slow_mode=true,
    .slow_mean_ms=55.0, .slow_jitter_ms=18.0, .spike_prob=0.03, .spike_ms=500.0};

// ── Helpers ─────────────────────────────────────────────────────────────────

static double percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    double idx = p * static_cast<double>(sorted.size() - 1);
    auto lo = static_cast<std::size_t>(idx);
    auto hi = lo + 1;
    if (hi >= sorted.size()) return sorted.back();
    double frac = idx - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

static ShardId make_shard(int stripe, int idx) { return stripe * (K + M) + idx; }

static ScoreParams locked_health_ec_params() {
    ScoreParams p;
    p.theta_S = 2.0;
    p.theta_D = 4.0;
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

// ── Layout ──────────────────────────────────────────────────────────────────

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

// ── Policy interface ────────────────────────────────────────────────────────

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
            if (cfg.scenario != "canonical_stress20")
                throw std::invalid_argument("invalid --scenario: " + cfg.scenario);
        } else if (arg == "--policy") {
            std::string value = require_value(arg);
            if (value == "all") {
                cfg.policy_selection = PolicySelection::All;
            } else {
                cfg.policy_selection = PolicySelection::Single;
                cfg.single_policy = parse_policy_kind(value);
            }
        } else if (arg == "--timeout-ms") {
            cfg.timeout_ms = parse_double_value(require_value(arg), arg);
            if (cfg.timeout_ms < 0.0)
                throw std::invalid_argument("--timeout-ms must be non-negative");
        } else if (arg == "--format") {
            std::string value = require_value(arg);
            if (value == "table") {
                cfg.format = OutputFormat::Table;
            } else if (value == "csv") {
                cfg.format = OutputFormat::Csv;
            } else {
                throw std::invalid_argument("invalid --format: " + value);
            }
        } else {
            throw std::invalid_argument("unknown argument: " + arg);
        }
    }
    return cfg;
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
                bool straggler = !in_k.count(sh);
                double shard_lat_eff = straggler
                    ? std::numeric_limits<double>::max() : lat;
                double win = (parity_in_k &&
                              parity_won(parity_lat, shard_lat_eff,
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

static double bandwidth_overhead_pct(const RunResult& r) {
    double baseline_reads = static_cast<double>(r.total_requests * K);
    return (static_cast<double>(r.issued_shard_reads) - baseline_reads)
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

static RunResult run_one_policy(const RuntimeConfig& runtime,
                                const Layout& layout,
                                const PolicyConfig& config)
{
    static const std::string TMP = "/tmp/healthec_compare_policies";
    DiskSimulator sim(TMP, NUM_DISKS, PROFILE_BASELINE, runtime.seed);
    configure_slow_disks(sim);
    auto workload = make_workload(runtime.num_stripes, runtime.zipf_s, runtime.seed);
    return run_policy(sim, layout, workload, config, runtime.num_reads);
}

static void print_table_header(const RuntimeConfig& runtime,
                               const ScoreParams& health_params)
{
    std::cout << "compare_policies"
              << "  scenario=" << runtime.scenario
              << "  seed=" << runtime.seed
              << "  reads=" << runtime.num_reads
              << "  stripes=" << runtime.num_stripes
              << "  zipf_s=" << runtime.zipf_s << "\n";
    std::cout << "Slow disks: disk" << MILD_DISK << "=mild  disk"
              << SEVERE_DISK << "=severe  ratio=20% stress\n";
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

static void print_csv_header()
{
    std::cout
        << "scenario,seed,num_reads,num_stripes,zipf_s,policy,timeout_ms,"
        << "p50_ms,p95_ms,p99_ms,p99_improvement_pct,issued_shard_reads,"
        << "bandwidth_overhead_pct,parity_reads,proactive_or_degraded_reads,"
        << "decode_count,migration_triggers,migration_false_positives\n";
}

static void print_csv_row(const RuntimeConfig& runtime,
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

// ── main ────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    RuntimeConfig runtime;
    try {
        runtime = parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "compare_policies: " << e.what() << "\n";
        return 1;
    }

    static const std::string TMP = "/tmp/healthec_compare_policies";
    std::filesystem::remove_all(TMP);
    std::filesystem::create_directories(TMP);

    const Layout layout = build_layout(runtime.num_stripes);
    const ScoreParams health_params = locked_health_ec_params();
    const auto policies = selected_policies(runtime);

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
        print_csv_header();
    }

    for (std::size_t i = 0; i < configs.size(); ++i) {
        if (runtime.format == OutputFormat::Table) {
            print_table_row(configs[i].kind, results[i], vanilla_p99);
        } else {
            print_csv_row(runtime, configs[i].kind, results[i], vanilla_p99);
        }
    }

    std::filesystem::remove_all(TMP);
    return 0;
}
