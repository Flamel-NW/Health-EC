#include "sim/disk_simulator.h"

#include <chrono>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;

namespace healthec::sim {

static void validate_profile(const DiskProfile& profile) {
    auto finite_nonnegative = [](double value) {
        return std::isfinite(value) && value >= 0.0;
    };

    if (!finite_nonnegative(profile.base_mean_ms) ||
        !finite_nonnegative(profile.base_jitter_ms) ||
        !finite_nonnegative(profile.slow_mean_ms) ||
        !finite_nonnegative(profile.slow_jitter_ms) ||
        !finite_nonnegative(profile.spike_ms) ||
        !std::isfinite(profile.spike_prob) ||
        profile.spike_prob < 0.0 || profile.spike_prob > 1.0) {
        throw std::invalid_argument("DiskSimulator: invalid DiskProfile");
    }
}

static double sample_normal_or_constant(std::mt19937& rng,
                                        double mean,
                                        double jitter) {
    if (jitter == 0.0)
        return mean;
    std::normal_distribution<double> normal(mean, jitter);
    return normal(rng);
}

// ── Constructor ───────────────────────────────────────────────────────────────

DiskSimulator::DiskSimulator(std::string base_dir, int num_disks,
                             DiskProfile default_profile, uint64_t seed)
    : base_dir_(std::move(base_dir)),
      num_disks_(num_disks),
      profiles_(num_disks > 0 ? num_disks : 0, default_profile),
      rng_(seed) {
    if (num_disks_ < 1)
        throw std::invalid_argument("num_disks must be >= 1");
    validate_profile(default_profile);
    fs::create_directories(base_dir_);
}

// ── Path helpers ──────────────────────────────────────────────────────────────

void DiskSimulator::validate_disk_id(core::DiskId disk) const {
    if (disk < 0 || disk >= num_disks_)
        throw std::out_of_range("DiskSimulator: DiskId out of range");
}

fs::path DiskSimulator::shard_path(core::DiskId disk, core::ShardId shard) const {
    validate_disk_id(disk);
    return fs::path(base_dir_) / ("disk" + std::to_string(disk))
                               / ("shard" + std::to_string(shard) + ".bin");
}

void DiskSimulator::ensure_disk_dir(core::DiskId disk) const {
    validate_disk_id(disk);
    fs::create_directories(
        fs::path(base_dir_) / ("disk" + std::to_string(disk)));
}

// ── write_shard ───────────────────────────────────────────────────────────────

void DiskSimulator::write_shard(core::DiskId disk, core::ShardId shard,
                                std::vector<uint8_t> data) {
    ensure_disk_dir(disk);
    auto path = shard_path(disk, shard);
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs)
        throw std::runtime_error("write_shard: cannot open " + path.string());
    if (!data.empty())
        ofs.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
}

// ── read_data ─────────────────────────────────────────────────────────────────

std::vector<uint8_t> DiskSimulator::read_data(core::DiskId disk,
                                              core::ShardId shard) const {
    auto path = shard_path(disk, shard);
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs)
        throw std::runtime_error("read_data: cannot open " + path.string());
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(ifs),
                                std::istreambuf_iterator<char>());
}

// ── sample_latency_ms ─────────────────────────────────────────────────────────

double DiskSimulator::sample_latency_ms(core::DiskId disk) {
    validate_disk_id(disk);
    DiskProfile profile;
    {
        std::shared_lock lock(profiles_mu_);
        profile = profiles_[disk];
    }

    double mean   = profile.slow_mode ? profile.slow_mean_ms   : profile.base_mean_ms;
    double jitter = profile.slow_mode ? profile.slow_jitter_ms : profile.base_jitter_ms;

    double lat;
    {
        std::lock_guard lg(rng_mu_);
        lat = sample_normal_or_constant(rng_, mean, jitter);

        if (profile.slow_mode && profile.spike_prob > 0.0) {
            std::bernoulli_distribution spike(profile.spike_prob);
            if (spike(rng_)) lat += profile.spike_ms;
        }
    }

    return std::max(0.0, lat);
}

// ── read_shard ────────────────────────────────────────────────────────────────

core::ShardReadResult DiskSimulator::read_shard(core::DiskId  disk,
                                               core::ShardId shard,
                                               bool          is_parity) {
    double delay_ms = sample_latency_ms(disk);

    // Inject the sampled delay before performing file I/O.
    std::this_thread::sleep_for(
        std::chrono::duration<double, std::milli>(delay_ms));

    // Perform actual file I/O (result discarded here; caller uses read_data()
    // separately when raw bytes are needed for EC decode).
    // We tolerate a missing file (shard not yet written) to keep tests simple.
    auto path = shard_path(disk, shard);
    if (fs::exists(path)) {
        std::ifstream ifs(path, std::ios::binary);
        // Consume the bytes to simulate a real read; ignore the data here.
        std::istreambuf_iterator<char> it(ifs), end;
        volatile std::size_t consumed = 0;
        for (; it != end; ++it) ++consumed;
        (void)consumed;
    }

    return {shard, is_parity, delay_ms};
}

// ── make_reader ───────────────────────────────────────────────────────────────

core::ShardReader DiskSimulator::make_reader() {
    return [this](core::DiskId disk, core::ShardId shard,
                  bool is_parity) -> core::ShardReadResult {
        return read_shard(disk, shard, is_parity);
    };
}

// ── migrate_shard / make_mover ────────────────────────────────────────────────

void DiskSimulator::migrate_shard(core::DiskId src, core::ShardId shard,
                                   core::DiskId dst) {
    auto from = shard_path(src, shard);
    auto to   = shard_path(dst, shard);
    if (!fs::exists(from))
        throw std::runtime_error("migrate_shard: source not found " + from.string());
    ensure_disk_dir(dst);
    fs::copy_file(from, to, fs::copy_options::overwrite_existing);
    fs::remove(from);
}

core::ShardMover DiskSimulator::make_mover() {
    return [this](core::DiskId src, core::ShardId shard, core::DiskId dst) {
        migrate_shard(src, shard, dst);
    };
}

// ── Profile control ───────────────────────────────────────────────────────────

void DiskSimulator::set_profile(core::DiskId disk, DiskProfile profile) {
    validate_disk_id(disk);
    validate_profile(profile);
    std::unique_lock lock(profiles_mu_);
    profiles_[disk] = profile;
}

DiskProfile DiskSimulator::get_profile(core::DiskId disk) const {
    validate_disk_id(disk);
    std::shared_lock lock(profiles_mu_);
    return profiles_[disk];
}

void DiskSimulator::set_slow(core::DiskId disk, bool slow) {
    validate_disk_id(disk);
    std::unique_lock lock(profiles_mu_);
    profiles_[disk].slow_mode = slow;
}

}  // namespace healthec::sim
