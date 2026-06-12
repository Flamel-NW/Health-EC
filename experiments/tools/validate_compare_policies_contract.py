#!/usr/bin/env python3
"""Contract checks for compare_policies dynamic output."""

import csv
import subprocess
import sys


POLICIES = [
    "vanilla_ec",
    "late_binding",
    "timeout_degraded_read",
    "health_ec",
]

DYNAMIC_HEADER = [
    "scenario",
    "seed",
    "num_reads",
    "num_stripes",
    "zipf_s",
    "num_windows",
    "window_size",
    "policy",
    "timeout_ms",
    "p50_ms",
    "p95_ms",
    "p99_ms",
    "p99_improvement_pct",
    "post_onset_p95_auc_ms",
    "post_onset_p99_auc_ms",
    "issued_shard_reads",
    "bandwidth_overhead_pct",
    "parity_reads",
    "pre_slowdown_parity_reads",
    "recovery_parity_reads",
    "proactive_or_degraded_reads",
    "decode_count",
    "migration_triggers",
    "migration_true_positives",
    "migration_false_positives",
    "migration_false_negatives",
    "first_detection_latency_reads",
    "first_mitigation_latency_reads",
    "recovery_regret_reads",
]

WINDOWED_HEADER = [
    "scenario",
    "seed",
    "num_reads",
    "num_stripes",
    "zipf_s",
    "policy",
    "timeout_ms",
    "window_id",
    "window_start_read",
    "window_end_read",
    "disk8_state",
    "disk9_state",
    "active_slow_disks",
    "p50_ms",
    "p95_ms",
    "p99_ms",
    "issued_shard_reads",
    "bandwidth_overhead_pct",
    "parity_reads",
    "proactive_or_degraded_reads",
    "decode_count",
    "migration_triggers",
    "migration_true_positives",
    "migration_false_positives",
]

EVENT_TRACE_HEADER = [
    "scenario",
    "seed",
    "event_id",
    "disk_id",
    "state",
    "start_window",
    "end_window",
    "start_read",
    "end_read",
    "is_migration_positive",
    "notes",
]


def fail(message):
    raise SystemExit(f"ERROR: {message}")


def run(cmd, expect_success=True, expected_stderr=None):
    result = subprocess.run(
        cmd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if expect_success:
        if result.returncode != 0:
            fail(f"{cmd} failed: {result.stderr.strip()}")
        if result.stderr:
            fail(f"{cmd} wrote stderr: {result.stderr.strip()}")
    else:
        if result.returncode == 0:
            fail(f"{cmd} unexpectedly succeeded")
        if expected_stderr and expected_stderr not in result.stderr:
            fail(
                f"{cmd} stderr mismatch: expected {expected_stderr!r}, "
                f"got {result.stderr!r}"
            )
    return result


def csv_rows(text, expected_header):
    reader = csv.DictReader(text.splitlines())
    if reader.fieldnames != expected_header:
        fail(f"unexpected header: {reader.fieldnames}")
    rows = list(reader)
    if not rows:
        fail("missing CSV rows")
    return rows


def check_aggregate(runner):
    result = run([
        runner,
        "--scenario",
        "dynamic_degradation",
        "--num-reads",
        "20000",
        "--policy",
        "all",
        "--format",
        "csv",
    ])
    rows = csv_rows(result.stdout, DYNAMIC_HEADER)
    if len(rows) != len(POLICIES):
        fail(f"expected 4 aggregate rows, got {len(rows)}")
    order = [row["policy"] for row in rows]
    if order != POLICIES:
        fail(f"unexpected aggregate policy order: {order}")


def check_windowed(runner):
    result = run([
        runner,
        "--scenario",
        "dynamic_degradation",
        "--num-reads",
        "20000",
        "--policy",
        "all",
        "--format",
        "windowed_csv",
    ])
    rows = csv_rows(result.stdout, WINDOWED_HEADER)
    if len(rows) != len(POLICIES) * 20:
        fail(f"expected 80 windowed rows, got {len(rows)}")
    policy_order = []
    for row in rows:
        if not policy_order or policy_order[-1] != row["policy"]:
            policy_order.append(row["policy"])
    if policy_order != POLICIES:
        fail(f"unexpected windowed policy order: {policy_order}")
    for policy in POLICIES:
        ids = [int(row["window_id"]) for row in rows if row["policy"] == policy]
        if ids != list(range(20)):
            fail(f"unexpected window ids for {policy}: {ids}")


def check_event_trace(runner):
    result = run([
        runner,
        "--scenario",
        "dynamic_degradation",
        "--num-reads",
        "20000",
        "--format",
        "event_trace",
    ])
    rows = csv_rows(result.stdout, EVENT_TRACE_HEADER)
    if len(rows) != 13:
        fail(f"expected 13 event trace rows, got {len(rows)}")


def check_negative_commands(runner):
    run(
        [
            runner,
            "--scenario",
            "dynamic_degradation",
            "--num-reads",
            "99999",
            "--format",
            "csv",
        ],
        expect_success=False,
        expected_stderr="divisible by 20",
    )
    run(
        [
            runner,
            "--scenario",
            "dynamic_degradation",
            "--num-reads",
            "20000",
            "--format",
            "csv",
            "--health-theta-s",
            "-1",
        ],
        expect_success=False,
        expected_stderr="must be positive",
    )
    run(
        [
            runner,
            "--scenario",
            "dynamic_degradation",
            "--num-reads",
            "20000",
            "--format",
            "csv",
            "--health-parity-win-abs-ms",
            "-1",
        ],
        expect_success=False,
        expected_stderr="must be non-negative",
    )


def main():
    if len(sys.argv) != 2:
        fail("usage: validate_compare_policies_contract.py <compare_policies>")
    runner = sys.argv[1]
    check_aggregate(runner)
    check_windowed(runner)
    check_event_trace(runner)
    check_negative_commands(runner)


if __name__ == "__main__":
    main()
