#include "workload_generator.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace healthec::sim {

WorkloadGenerator::WorkloadGenerator(int num_stripes, double zipf_s, uint64_t seed)
    : num_stripes_(num_stripes), rng_(seed)
{
    if (num_stripes <= 0)
        throw std::invalid_argument("WorkloadGenerator: num_stripes must be > 0");
    if (zipf_s < 0.0)
        throw std::invalid_argument("WorkloadGenerator: zipf_s must be >= 0");

    // Compute raw PMF weights: w[i] = 1 / (i+1)^s
    std::vector<double> pmf(num_stripes);
    for (int i = 0; i < num_stripes; ++i)
        pmf[i] = 1.0 / std::pow(static_cast<double>(i + 1), zipf_s);

    // Normalise to get true PMF and build CDF
    double Z = 0.0;
    for (double v : pmf) Z += v;

    cdf_.resize(num_stripes);
    weights_.resize(num_stripes);
    double cumulative = 0.0;
    for (int i = 0; i < num_stripes; ++i) {
        double p  = pmf[i] / Z;
        cumulative += p;
        cdf_[i]     = cumulative;
        weights_[i] = p * static_cast<double>(num_stripes);  // mean ≈ 1.0
    }
    // Guard against floating-point rounding: ensure last bucket covers [0,1]
    cdf_.back() = 1.0;
}

core::StripeId WorkloadGenerator::next_stripe()
{
    double u = uniform_(rng_);
    // upper_bound returns iterator to first element > u → gives 0-based index
    auto it = std::upper_bound(cdf_.begin(), cdf_.end(), u);
    if (it == cdf_.end()) --it;
    return static_cast<core::StripeId>(std::distance(cdf_.begin(), it));
}

double WorkloadGenerator::hotness(core::StripeId stripe_id) const
{
    if (stripe_id < 0 || stripe_id >= num_stripes_)
        return 0.0;
    return weights_[stripe_id];
}

}  // namespace healthec::sim
