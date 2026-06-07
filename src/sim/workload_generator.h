#pragma once

#include "core/read_scheduler.h"  // StripeId

#include <cstdint>
#include <random>
#include <vector>

namespace healthec::sim {

// Generates stripe access requests following a Zipf power-law distribution,
// simulating the skewed access patterns observed in real storage workloads.
//
// Zipf PMF:  P(i) = (1 / (i+1)^s) / Z_n
//            Z_n  = sum_{j=0}^{n-1} 1/(j+1)^s
//
// Stripe IDs are 0-based.  Stripe 0 is the hottest (most frequently accessed).
//
// Thread-safety: next_stripe() is NOT thread-safe; use external synchronisation
// if called from multiple threads.
class WorkloadGenerator {
public:
    // num_stripes : total number of stripes in the system
    // zipf_s      : Zipf exponent (1.0 = typical storage skew; higher = more skewed)
    // seed        : RNG seed for reproducibility
    WorkloadGenerator(int num_stripes, double zipf_s = 1.0, uint64_t seed = 42);

    // Return the next stripe ID sampled from the Zipf distribution.
    // Uses O(log n) inverse-CDF lookup via std::upper_bound.
    core::StripeId next_stripe();

    // Return the hotness weight for stripe_id, for use as w_s in
    // ScoreManager::update_slowness() / update_death().
    // Normalised so that the mean weight across all stripes ≈ 1.0.
    double hotness(core::StripeId stripe_id) const;

    int num_stripes() const noexcept { return num_stripes_; }

private:
    int                 num_stripes_;
    std::vector<double> cdf_;      // cumulative distribution; cdf_[i] = P(X <= i)
    std::vector<double> weights_;  // normalised hotness: weights_[i] = P(i) * num_stripes
    std::mt19937_64     rng_;
    std::uniform_real_distribution<double> uniform_{0.0, 1.0};
};

}  // namespace healthec::sim
