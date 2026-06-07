# Health-EC

**Health-Aware Erasure Coding** — a C++ prototype that mitigates tail latency
caused by fail-slow disks in erasure-coded storage, without triggering
full-disk replacement.

The system uses a three-tier scoring framework operating at shard granularity:

| Score | Symbol | Purpose |
|---|---|---|
| Health Score | H_d | Placement: prefer high-health disks on write |
| Slowness Score | S_i | Proactive parity read: bypass straggler shards |
| Death Score | D_i | Selective shard migration: move only affected shards |

## Dependencies

| Library | Version | Install |
|---|---|---|
| CMake | ≥ 3.16 | `apt install cmake` |
| g++ | ≥ 11 (C++17) | `apt install g++` |
| Jerasure 2.x | ≥ 2.0 | `apt install libjerasure-dev` |
| gf-complete | ≥ 1.0.2 | `apt install libgf-complete-dev` |

Install all at once:

```bash
sudo apt install cmake g++ libjerasure-dev libgf-complete-dev
```

> **Note (Debian/Ubuntu packaging quirk)**: the Jerasure shared library is
> installed as `libJerasure.so` (capital J).  The `cmake/FindJerasure.cmake`
> module handles this automatically.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Test

```bash
ctest --test-dir build --output-on-failure
```

Expected output:

```
Codec: RS(4,2) w=8 shard_size=4096
PASS [no_erasure]
PASS [erase_2_data]
PASS [erase_1data_1parity]
PASS [erase_2_parity]
All tests passed.
```

## Directory layout

```
Health-EC/
├── cmake/
│   └── FindJerasure.cmake          # locates libJerasure + libgf_complete
├── src/
│   ├── ec/
│   │   ├── jerasure_wrapper.h/.cpp # RS encode/decode (JerasureCodec)
│   │   └── CMakeLists.txt
│   ├── core/                       # ScoreManager / PlacementPolicy / ...  [T1.2+]
│   ├── sim/                        # DiskSimulator / WorkloadGenerator      [T1.3+]
│   └── CMakeLists.txt
├── tests/
│   ├── test_jerasure_wrapper.cpp
│   └── CMakeLists.txt
├── experiments/                    # configs + analysis scripts             [T2+]
├── CMakeLists.txt
└── README.md
```

## Architecture overview

```
WorkloadGenerator
  └─→ ReadScheduler           (proactive parity read, first-k-complete)
        └─→ ScoreManager      (H_d / S_i / D_i EMA updates)
        └─→ DiskSimulator     (latency injection, shard I/O)
              └─→ JerasureCodec   (Reed-Solomon encode / decode)
MigrationScheduler            (budgeted shard migration driven by D_i)
```

## License

Source code: MIT.  Jerasure and gf-complete are BSD-licensed (James Plank,
University of Tennessee).
