#pragma once

#include <vector>
#include <stdexcept>

namespace healthec::ec {

// EC encoding parameters.  (k, m) is the standard notation used throughout
// the codebase (see naming_conventions.md §2).
struct EcParams {
    int k;        // number of data shards
    int m;        // number of parity shards
    int w = 8;    // Galois Field word size in bits; 8 works for any (k+m) <= 256
};

// Reed-Solomon codec backed by Jerasure 2.x (Vandermonde matrix variant).
//
// Thread-safety: encode/decode are read-only with respect to internal state
// and may be called concurrently.  The constructor/destructor are not
// thread-safe.
class JerasureCodec {
public:
    // Constructs the codec and pre-computes the k×m Vandermonde coding matrix.
    // Throws std::invalid_argument if params are out of range for Jerasure.
    explicit JerasureCodec(EcParams params);

    JerasureCodec(const JerasureCodec&)            = delete;
    JerasureCodec& operator=(const JerasureCodec&) = delete;
    JerasureCodec(JerasureCodec&&)                 = delete;
    JerasureCodec& operator=(JerasureCodec&&)      = delete;

    ~JerasureCodec();

    // Encodes k data shards into m parity shards.
    //
    // Preconditions:
    //   - data_shards.size() == k
    //   - every shard has the same size, which must be a multiple of sizeof(long)
    //
    // Returns a vector of m parity shards (each the same size as the input shards).
    std::vector<std::vector<char>> encode(
        const std::vector<std::vector<char>>& data_shards) const;

    // Reconstructs erased shards in-place.
    //
    // all_shards must contain exactly k+m slots (data shards first, then parity
    // shards).  Slots listed in erased_indices are treated as lost; their content
    // on input is ignored and they are filled in on success.
    //
    // Returns the k data shards (copied out of all_shards after reconstruction).
    // Throws std::runtime_error if decoding fails (e.g. too many erasures).
    std::vector<std::vector<char>> decode(
        std::vector<std::vector<char>>& all_shards,
        const std::vector<int>& erased_indices) const;

    const EcParams& params() const noexcept { return params_; }

private:
    EcParams params_;
    int*     matrix_;  // Vandermonde coding matrix, size k*m, allocated by Jerasure
};

}  // namespace healthec::ec
