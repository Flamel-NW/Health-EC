#pragma once

#include "score_manager.h"

#include <vector>

namespace healthec::core {

// Selects disks for new EC stripe placement based on Health Score H_d.
//
// The placement strategy (§2 of design/scoring-algorithms.md):
//   Sort all registered disks by H_d descending, pick the top n = k+m.
// This steers new writes away from degraded disks without any disk-level
// eviction — satisfying the coarse-granularity constraint.
class PlacementPolicy {
public:
    // score_manager must outlive this object.
    explicit PlacementPolicy(const ScoreManager& score_manager);

    // Returns the k+m disk IDs with the highest H_d scores, in descending order.
    //
    // Throws std::invalid_argument if fewer than k+m disks are registered in
    // the ScoreManager (i.e. no health update has been issued for them yet).
    std::vector<DiskId> select_disks(int k, int m) const;

private:
    const ScoreManager& score_manager_;
};

}  // namespace healthec::core
