# Health-EC

**Health-Aware Erasure Coding** — a C++ prototype that mitigates tail latency
caused by fail-slow disks in erasure-coded storage, without triggering
full-disk replacement.

T4.1.13.1 的修复版本位于 [maintenance/](maintenance/README.md)，包含独立源码、
构建入口和回归测试。下述命令使用该维护版本；原根目录实现保留用于历史复现。

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
cmake -S maintenance -B build-maintenance -DCMAKE_BUILD_TYPE=Release
cmake --build build-maintenance -j$(nproc)
```

## Test

```bash
ctest --test-dir build-maintenance --output-on-failure
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

## Simulation traces

For passive per-disk observations from the dynamic simulation:

```bash
build-maintenance/experiments/compare_policies \
  --scenario dynamic_realistic_100d_2pct_hdd \
  --num-reads 1000000 --num-stripes 5000 --seed 42 \
  --policy health_ec --health-migration-strategy disabled \
  --format disk_trace --detector-bin-reads 10000 > disk_trace.csv
```

Each row reports one disk/request-index bin: actual issued data/parity reads,
mean simulated latency, end-of-bin maximum shard S/D scores, and validation
labels. Bins must divide the schedule window size. An empty latency means no
reads were issued. Observations do not change policy decisions; ground-truth
and bypass labels are for offline validation. `validation_bypass_*` counts
positive data-shard exposures, which can exceed request counts when a request
contains multiple slow shards. The simulator has no latency/queue-pressure
coupling and exposes full issued-read latency without cancellation censoring.

## Directory layout

```
Health-EC/
├── maintenance/                    # maintained source, CMake, and regression tests
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
