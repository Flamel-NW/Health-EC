#!/usr/bin/env python3
"""Verify issued-read accounting and passive disk trace output."""
import csv
import io
import math
import subprocess
import sys


def run(runner, args, success=True):
    result = subprocess.run([runner, *args], capture_output=True, text=True)
    assert (result.returncode == 0) == success, result.stderr
    return list(csv.DictReader(io.StringIO(result.stdout))) if success else []


def main():
    runner = sys.argv[1]
    for scenario, disks in [("dynamic_degradation", 10),
                            ("dynamic_realistic_100d_2pct_hdd", 100)]:
        args = ["--scenario", scenario, "--num-reads", "20000", "--num-stripes", "500",
                "--health-migration-strategy", "disabled"]
        aggregate = run(runner, args + ["--format", "csv"])
        trace = run(runner, args + ["--format", "disk_trace", "--detector-bin-reads", "1000"])
        assert len(trace) == 4 * disks * 20
        keys = {(r["policy"], int(r["disk_id"]), int(r["detector_window"])) for r in trace}
        assert len(keys) == len(trace)
        for row in aggregate:
            rows = [r for r in trace if r["policy"] == row["policy"]]
            assert sum(int(r["read_count"]) for r in rows) == int(row["issued_shard_reads"])
            assert sum(int(r["data_read_count"]) for r in rows) == 4 * 20000
            assert sum(int(r["parity_read_count"]) for r in rows) == int(row["parity_reads"])
            assert sum(int(r["migration_count"]) for r in rows) == int(row["migration_triggers"])
            for r in rows:
                count = int(r["read_count"])
                assert count == int(r["data_read_count"]) + int(r["parity_read_count"])
                assert int(r["read_bytes"]) == count * 4096
                assert float(r["pressure_proxy"]) == count
                assert int(r["window_end_read"]) - int(r["window_start_read"]) == 1000
                if count:
                    assert math.isfinite(float(r["observed_latency_ms"]))
                else:
                    assert r["observed_latency_ms"] == ""
                assert int(r["validation_bypass_triggered"]) <= int(r["validation_bypass_eligible"])
    base = ["--scenario", "dynamic_degradation", "--num-reads", "20000", "--format", "disk_trace"]
    for invalid in ["0", "-1", "3", "2000"]:
        run(runner, base + ["--detector-bin-reads", invalid], success=False)
    run(runner, ["--format", "disk_trace"], success=False)
    print("disk trace schema / issued-read conservation / invalid bins PASS")


if __name__ == "__main__":
    main()
