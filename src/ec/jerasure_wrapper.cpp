#include "jerasure_wrapper.h"

#include <jerasure.h>
#include <jerasure/reed_sol.h>

#include <cassert>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace healthec::ec {

JerasureCodec::JerasureCodec(EcParams params) : params_(params), matrix_(nullptr) {
    if (params_.k <= 0 || params_.m <= 0) {
        throw std::invalid_argument("k and m must be positive");
    }
    if (params_.w != 8 && params_.w != 16 && params_.w != 32) {
        throw std::invalid_argument("w must be 8, 16, or 32 for matrix coding");
    }
    if (params_.k + params_.m > (1 << params_.w)) {
        throw std::invalid_argument("k+m must not exceed 2^w");
    }

    // reed_sol_vandermonde_coding_matrix returns a heap-allocated int array of
    // size k*m.  Jerasure owns the allocation; we take ownership and free it
    // in the destructor via free() (Jerasure uses malloc internally).
    matrix_ = reed_sol_vandermonde_coding_matrix(params_.k, params_.m, params_.w);
    if (!matrix_) {
        throw std::runtime_error("reed_sol_vandermonde_coding_matrix failed");
    }
}

JerasureCodec::~JerasureCodec() {
    free(matrix_);
}

std::vector<std::vector<char>> JerasureCodec::encode(
    const std::vector<std::vector<char>>& data_shards) const
{
    const int k = params_.k;
    const int m = params_.m;
    const int w = params_.w;

    if (static_cast<int>(data_shards.size()) != k) {
        throw std::invalid_argument(
            "encode: expected " + std::to_string(k) + " data shards, got " +
            std::to_string(data_shards.size()));
    }

    const int shard_size = static_cast<int>(data_shards[0].size());
    if (shard_size == 0 || shard_size % static_cast<int>(sizeof(long)) != 0) {
        throw std::invalid_argument(
            "encode: shard_size must be a non-zero multiple of sizeof(long)");
    }
    for (int i = 1; i < k; ++i) {
        if (static_cast<int>(data_shards[i].size()) != shard_size) {
            throw std::invalid_argument("encode: all data shards must have the same size");
        }
    }

    // Jerasure needs non-const char* pointers; copy input into mutable buffers.
    std::vector<std::vector<char>> data_bufs(data_shards);
    std::vector<std::vector<char>> parity_shards(m, std::vector<char>(shard_size, 0));

    std::vector<char*> data_ptrs(k);
    std::vector<char*> coding_ptrs(m);
    for (int i = 0; i < k; ++i) data_ptrs[i]   = data_bufs[i].data();
    for (int i = 0; i < m; ++i) coding_ptrs[i] = parity_shards[i].data();

    jerasure_matrix_encode(k, m, w, matrix_,
                           data_ptrs.data(), coding_ptrs.data(), shard_size);

    return parity_shards;
}

std::vector<std::vector<char>> JerasureCodec::decode(
    std::vector<std::vector<char>>& all_shards,
    const std::vector<int>& erased_indices) const
{
    const int k = params_.k;
    const int m = params_.m;
    const int w = params_.w;
    const int n = k + m;

    if (static_cast<int>(all_shards.size()) != n) {
        throw std::invalid_argument(
            "decode: all_shards must contain k+m=" + std::to_string(n) + " slots");
    }

    // Infer shard_size from the first non-erased, non-empty slot.
    // Avoids silently using size=0 when shard 0 is an erased slot (M2).
    std::unordered_set<int> erased_set(erased_indices.begin(), erased_indices.end());
    int shard_size = 0;
    for (int i = 0; i < n; ++i) {
        if (!erased_set.count(i) && !all_shards[i].empty()) {
            shard_size = static_cast<int>(all_shards[i].size());
            break;
        }
    }
    if (shard_size == 0) {
        throw std::invalid_argument(
            "decode: cannot infer shard_size (all present shards are empty)");
    }
    if (shard_size % static_cast<int>(sizeof(long)) != 0) {
        throw std::invalid_argument(
            "decode: shard_size must be a multiple of sizeof(long)");
    }

    // Ensure erased slots have a buffer of the right size (content will be overwritten).
    for (int idx : erased_indices) {
        if (idx < 0 || idx >= n) {
            throw std::invalid_argument("decode: erased index out of range");
        }
        all_shards[idx].assign(shard_size, 0);
    }

    std::vector<char*> data_ptrs(k);
    std::vector<char*> coding_ptrs(m);
    for (int i = 0; i < k; ++i) data_ptrs[i]   = all_shards[i].data();
    for (int i = 0; i < m; ++i) coding_ptrs[i] = all_shards[k + i].data();

    // erasures array: list of erased IDs terminated by -1.
    std::vector<int> erasures(erased_indices);
    erasures.push_back(-1);

    // row_k_ones=1 for Vandermonde matrix (first row of coding matrix is all 1s).
    int rc = jerasure_matrix_decode(k, m, w, matrix_,
                                    /*row_k_ones=*/1,
                                    erasures.data(),
                                    data_ptrs.data(), coding_ptrs.data(), shard_size);
    if (rc != 0) {
        throw std::runtime_error("jerasure_matrix_decode failed (too many erasures?)");
    }

    std::vector<std::vector<char>> result(k);
    for (int i = 0; i < k; ++i) result[i] = all_shards[i];
    return result;
}

}  // namespace healthec::ec
