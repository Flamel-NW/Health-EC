#pragma once

#include "core/migration_scheduler.h"  // ShardMover
#include "core/read_scheduler.h"       // ShardReadResult, ShardReader, DiskId, ShardId

#include <filesystem>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <string>
#include <vector>

namespace healthec::sim {

// Per-disk latency model parameters.
// All fields are mutable at runtime via DiskSimulator::set_profile().
//
// Normal mode  : latency ~ max(0, N(base_mean_ms, base_jitter_ms))
// Slow mode    : latency ~ max(0, N(slow_mean_ms, slow_jitter_ms))
//                        + Bernoulli(spike_prob) × spike_ms
//
// Thresholds must be tuned experimentally (T2); defaults are illustrative.
struct DiskProfile {
    double base_mean_ms   =  5.0;   // μ_d  : mean latency, normal mode
    double base_jitter_ms =  1.0;   // σ_d  : std-deviation, normal mode
    bool   slow_mode      = false;  // fail-slow enabled?
    double slow_mean_ms   = 50.0;   // mean latency, slow mode
    double slow_jitter_ms = 20.0;   // std-deviation, slow mode
    double spike_prob     =  0.0;   // per-read spike probability [0, 1]
    double spike_ms       = 200.0;  // extra latency added on a spike
};

// Simulates a set of virtual disks backed by local filesystem directories.
//
// Each disk maps to  <base_dir>/disk{d}/
// Each shard file is <base_dir>/disk{d}/shard{i}.bin
//
// Latency is injected via std::this_thread::sleep_for() inside read_shard().
// write_shard() / read_data() perform plain file I/O without delay.
//
// Thread-safety: all public methods are safe to call concurrently.
class DiskSimulator {
public:
    // base_dir   : root directory for virtual disks (created if absent)
    // num_disks  : number of virtual disks (DiskId must be in [0, num_disks))
    // default_profile : initial DiskProfile applied to every disk
    explicit DiskSimulator(std::string    base_dir,
                           int            num_disks,
                           DiskProfile    default_profile = DiskProfile{},
                           uint64_t       seed            = std::random_device{}());

    // Write shard data to disk.  Creates <base_dir>/disk{d}/ as needed.
    void write_shard(core::DiskId disk, core::ShardId shard,
                     std::vector<uint8_t> data);

    // Read raw shard bytes without injecting delay.
    // Used by the EC decode path (T1.5+).  Throws std::runtime_error on I/O failure.
    std::vector<uint8_t> read_data(core::DiskId disk, core::ShardId shard) const;

    // Inject latency (sampled from disk's DiskProfile), then read the shard file.
    // Returns a ShardReadResult where latency_ms == injected delay.
    // Satisfies the healthec::core::ShardReader signature exactly.
    core::ShardReadResult read_shard(core::DiskId  disk,
                                     core::ShardId shard,
                                     bool          is_parity);

    // Returns a ShardReader lambda bound to *this.
    // Pass directly to ReadScheduler as the reader callback.
    // IMPORTANT: DiskSimulator must outlive all ReadScheduler instances that use
    // this reader; the lambda captures a raw `this` pointer.
    core::ShardReader make_reader();

    // Move shard `shard` from disk `src` to disk `dst`.
    // Copies the shard file to the destination directory then removes the source.
    // Throws std::runtime_error if the source shard file does not exist.
    void migrate_shard(core::DiskId src, core::ShardId shard, core::DiskId dst);

    // Returns a ShardMover lambda bound to *this.
    // Pass directly to MigrationScheduler as the mover callback.
    core::ShardMover make_mover();

    // ── Runtime profile control ───────────────────────────────────────────────

    // Replace the full DiskProfile for disk d.
    void        set_profile(core::DiskId disk, DiskProfile profile);
    DiskProfile get_profile(core::DiskId disk) const;

    // Toggle slow mode without changing other DiskProfile fields.
    void set_slow(core::DiskId disk, bool slow);

    int num_disks() const noexcept { return num_disks_; }

    // Sample a non-negative latency (ms) for the given disk without performing I/O.
    // Acquires rng_mu_ internally; caller must NOT hold rng_mu_.
    // Exposed as public for use by calibration tools (e.g. experiments/tools/calibrate_params).
    double sample_latency_ms(core::DiskId disk);

private:
    std::string  base_dir_;
    int          num_disks_;

    mutable std::shared_mutex   profiles_mu_;
    std::vector<DiskProfile>    profiles_;     // indexed by DiskId

    mutable std::mutex          rng_mu_;
    std::mt19937                rng_;

    // Returns the filesystem path for a shard file.
    std::filesystem::path shard_path(core::DiskId disk, core::ShardId shard) const;

    // Creates <base_dir>/disk{d}/ if it does not exist.
    void ensure_disk_dir(core::DiskId disk) const;
};

}  // namespace healthec::sim
