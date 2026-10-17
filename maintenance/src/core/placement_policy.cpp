#include "placement_policy.h"

#include <stdexcept>
#include <string>

namespace healthec::core {

PlacementPolicy::PlacementPolicy(const ScoreManager& score_manager)
    : score_manager_(score_manager) {}

std::vector<DiskId> PlacementPolicy::select_disks(int k, int m) const {
    if (k <= 0 || m <= 0)
        throw std::invalid_argument("k and m must be positive");

    std::vector<DiskId> sorted = score_manager_.get_all_disks_sorted_by_health();
    int needed = k + m;
    if (static_cast<int>(sorted.size()) < needed) {
        throw std::invalid_argument(
            "not enough registered disks: need " + std::to_string(needed) +
            ", have " + std::to_string(sorted.size()));
    }

    sorted.resize(static_cast<std::size_t>(needed));
    return sorted;
}

}  // namespace healthec::core
