#include "ec/jerasure_wrapper.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using healthec::ec::EcParams;
using healthec::ec::JerasureCodec;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::vector<std::vector<char>> make_random_data(int k, int shard_size) {
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, 255);

    std::vector<std::vector<char>> shards(k, std::vector<char>(shard_size));
    for (auto& s : shards)
        for (auto& b : s)
            b = static_cast<char>(dist(rng));
    return shards;
}

// Builds the full k+m shard array (data + parity) ready for decode().
static std::vector<std::vector<char>> make_all_shards(
    const std::vector<std::vector<char>>& data_shards,
    const std::vector<std::vector<char>>& parity_shards)
{
    std::vector<std::vector<char>> all;
    all.insert(all.end(), data_shards.begin(),   data_shards.end());
    all.insert(all.end(), parity_shards.begin(), parity_shards.end());
    return all;
}

static void assert_data_eq(const std::vector<std::vector<char>>& original,
                            const std::vector<std::vector<char>>& recovered,
                            const char* label)
{
    assert(original.size() == recovered.size());
    for (size_t i = 0; i < original.size(); ++i) {
        if (original[i] != recovered[i]) {
            std::fprintf(stderr, "FAIL [%s]: shard %zu mismatch\n", label, i);
            std::abort();
        }
    }
    std::printf("PASS [%s]\n", label);
}

// ---------------------------------------------------------------------------
// Test cases
// ---------------------------------------------------------------------------

// Erasing 2 data shards (indices 0 and 1).
static void test_erase_2_data(const JerasureCodec& codec,
                               const std::vector<std::vector<char>>& data,
                               const std::vector<std::vector<char>>& parity)
{
    auto all = make_all_shards(data, parity);
    auto recovered = codec.decode(all, {0, 1});
    assert_data_eq(data, recovered, "erase_2_data");
}

// Erasing 1 data shard (index 2) and 1 parity shard (index k+0 = 4).
static void test_erase_1data_1parity(const JerasureCodec& codec,
                                      const std::vector<std::vector<char>>& data,
                                      const std::vector<std::vector<char>>& parity)
{
    const int k = codec.params().k;
    auto all = make_all_shards(data, parity);
    auto recovered = codec.decode(all, {2, k});    // k == parity shard 0
    assert_data_eq(data, recovered, "erase_1data_1parity");
}

// Erasing both parity shards (indices k and k+1).
static void test_erase_2_parity(const JerasureCodec& codec,
                                  const std::vector<std::vector<char>>& data,
                                  const std::vector<std::vector<char>>& parity)
{
    const int k = codec.params().k;
    auto all = make_all_shards(data, parity);
    auto recovered = codec.decode(all, {k, k + 1});
    assert_data_eq(data, recovered, "erase_2_parity");
}

// Sanity check: round-trip with no erasures.
static void test_no_erasure(const JerasureCodec& codec,
                              const std::vector<std::vector<char>>& data,
                              const std::vector<std::vector<char>>& parity)
{
    auto all = make_all_shards(data, parity);
    auto recovered = codec.decode(all, {});
    assert_data_eq(data, recovered, "no_erasure");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    // Parameters: k=4, m=2, w=8, shard_size=4096 (multiple of sizeof(long)=8)
    constexpr int k          = 4;
    constexpr int m          = 2;
    constexpr int w          = 8;
    constexpr int shard_size = 4096;

    static_assert(shard_size % sizeof(long) == 0,
                  "shard_size must be a multiple of sizeof(long)");

    EcParams params{k, m, w};
    JerasureCodec codec(params);

    auto data   = make_random_data(k, shard_size);
    auto parity = codec.encode(data);

    assert(static_cast<int>(parity.size()) == m);
    // All parity shards are produced by encode() with the same fixed size.
    assert(!parity.empty() && static_cast<int>(parity[0].size()) == shard_size);

    std::printf("Codec: RS(%d,%d) w=%d shard_size=%d\n", k, m, w, shard_size);

    test_no_erasure(codec, data, parity);
    test_erase_2_data(codec, data, parity);
    test_erase_1data_1parity(codec, data, parity);
    test_erase_2_parity(codec, data, parity);

    std::puts("All tests passed.");
    return 0;
}
