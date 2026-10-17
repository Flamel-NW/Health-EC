#include "sim/disk_simulator.h"

#include <chrono>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <system_error>

namespace fs = std::filesystem;

namespace healthec::sim {

namespace {
void sync_directory(const fs::path& path) {
    int fd=::open(path.c_str(),O_RDONLY|O_DIRECTORY);
    if(fd<0) throw std::runtime_error("cannot open parent directory");
    int result=::fsync(fd);int saved=errno;::close(fd);
    if(result) throw std::system_error(saved,std::generic_category(),"directory fsync");
}
std::vector<uint8_t> checked_read(const fs::path& path) {
    int fd=::open(path.c_str(),O_RDONLY|O_NOFOLLOW);
    if(fd<0) throw std::system_error(errno,std::generic_category(),"open shard");
    std::vector<uint8_t> bytes;uint8_t buffer[8192];
    try {
        for(;;){
            ssize_t n=::read(fd,buffer,sizeof(buffer));
            if(n<0 && errno==EINTR)continue;
            if(n<0)throw std::system_error(errno,std::generic_category(),"read shard");
            if(!n)break;
            bytes.insert(bytes.end(),buffer,buffer+n);
        }
    }catch(...){::close(fd);throw;}
    if(::close(fd))throw std::system_error(errno,std::generic_category(),"close shard");
    return bytes;
}
void checked_write(const fs::path& path,const std::vector<uint8_t>& bytes) {
    std::string pattern=(path.parent_path()/("."+path.filename().string()+".XXXXXX")).string();
    std::vector<char> name(pattern.begin(),pattern.end());name.push_back(0);
    int fd=::mkstemp(name.data());
    if(fd<0)throw std::system_error(errno,std::generic_category(),"stage shard");
    fs::path staged(name.data());
    try {
        std::size_t offset=0;
        while(offset<bytes.size()){
            ssize_t n=::write(fd,bytes.data()+offset,bytes.size()-offset);
            if(n<0 && errno==EINTR)continue;
            if(n<=0)throw std::system_error(n<0?errno:EIO,std::generic_category(),"write shard");
            offset+=static_cast<std::size_t>(n);
        }
        if(::fsync(fd))throw std::system_error(errno,std::generic_category(),"fsync shard");
        int closing=fd;fd=-1;
        if(::close(closing))throw std::system_error(errno,std::generic_category(),"close staged shard");
        fs::rename(staged,path);sync_directory(path.parent_path());
    }catch(...){
        if(fd>=0)::close(fd);
        std::error_code ignored;fs::remove(staged,ignored);throw;
    }
}
}


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

void DiskSimulator::write_shard(core::DiskId disk,core::ShardId shard,std::vector<uint8_t> data) {
    std::lock_guard guard(shard_locks_[static_cast<unsigned>(shard)%shard_locks_.size()]);
    ensure_disk_dir(disk);checked_write(shard_path(disk,shard),data);
}

// ── read_data ─────────────────────────────────────────────────────────────────

std::vector<uint8_t> DiskSimulator::read_data(core::DiskId disk,core::ShardId shard) const {
    std::lock_guard guard(shard_locks_[static_cast<unsigned>(shard)%shard_locks_.size()]);
    return checked_read(shard_path(disk,shard));
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
        lat = std::max(0.0, sample_normal_or_constant(rng_, mean, jitter));

        if (profile.slow_mode && profile.spike_prob > 0.0) {
            std::bernoulli_distribution spike(profile.spike_prob);
            if (spike(rng_)) lat += profile.spike_ms;
        }
    }

    if(!std::isfinite(lat))throw std::overflow_error("nonfinite sampled latency");
    return lat;
}

// ── read_shard ────────────────────────────────────────────────────────────────

core::ShardReadResult DiskSimulator::read_shard(core::DiskId disk,core::ShardId shard,bool parity) {
    double delay=sample_latency_ms(disk);
    std::this_thread::sleep_for(std::chrono::duration<double,std::milli>(delay));
    std::lock_guard guard(shard_locks_[static_cast<unsigned>(shard)%shard_locks_.size()]);
    auto path=shard_path(disk,shard);
    if(fs::exists(path))checked_read(path);
    return {shard,parity,delay};
}

// ── make_reader ───────────────────────────────────────────────────────────────

core::ShardReader DiskSimulator::make_reader() {
    return [this](core::DiskId disk, core::ShardId shard,
                  bool is_parity) -> core::ShardReadResult {
        return read_shard(disk, shard, is_parity);
    };
}

// ── migrate_shard / make_mover ────────────────────────────────────────────────

void DiskSimulator::migrate_shard(core::DiskId src,core::ShardId shard,core::DiskId dst) {
    std::lock_guard guard(shard_locks_[static_cast<unsigned>(shard)%shard_locks_.size()]);
    auto from=shard_path(src,shard);auto to=shard_path(dst,shard);
    auto data=checked_read(from);
    if(src==dst)return;
    ensure_disk_dir(dst);checked_write(to,data);
    if(!fs::remove(from))throw std::runtime_error("source disappeared during migration");
    sync_directory(from.parent_path());
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
