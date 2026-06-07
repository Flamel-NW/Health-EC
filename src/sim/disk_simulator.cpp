#include "sim/disk_simulator.h"

#include <chrono>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;

namespace healthec::sim {

// ── Constructor ───────────────────────────────────────────────────────────────

DiskSimulator::DiskSimulator(std::string base_dir, int num_disks,
                             DiskProfile default_profile)
    : base_dir_(std::move(base_dir)),
      num_disks_(num_disks),
      profiles_(num_disks, default_profile),
      rng_(std::random_device{}()) {
    if (num_disks_ < 1)
        throw std::invalid_argument("num_disks must be >= 1");
    fs::create_directories(base_dir_);
}

// ── Path helpers ──────────────────────────────────────────────────────────────

fs::path DiskSimulator::shard_path(core::DiskId disk, core::ShardId shard) const {
    return fs::path(base_dir_) / ("disk" + std::to_string(disk))
                               / ("shard" + std::to_string(shard) + ".bin");
}

void DiskSimulator::ensure_disk_dir(core::DiskId disk) const {
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
        std::normal_distribution<double> normal(mean, jitter);
        lat = normal(rng_);

        if (profile.spike_prob > 0.0) {
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

// ── Profile control ───────────────────────────────────────────────────────────

void DiskSimulator::set_profile(core::DiskId disk, DiskProfile profile) {
    std::unique_lock lock(profiles_mu_);
    profiles_[disk] = profile;
}

DiskProfile DiskSimulator::get_profile(core::DiskId disk) const {
    std::shared_lock lock(profiles_mu_);
    return profiles_[disk];
}

void DiskSimulator::set_slow(core::DiskId disk, bool slow) {
    std::unique_lock lock(profiles_mu_);
    profiles_[disk].slow_mode = slow;
}

}  // namespace healthec::sim
