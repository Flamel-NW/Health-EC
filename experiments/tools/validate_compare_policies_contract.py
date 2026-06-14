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
    "num_disks",
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

EXTENDED_DYNAMIC_HEADER = DYNAMIC_HEADER + [
    "post_warmup_windowed_p99_ms",
    "severe_window_p99_ms",
    "read_bypass_eligible_events",
    "read_bypass_triggered_events",
    "read_bypass_coverage_pct",
]

WINDOWED_HEADER = [
    "scenario",
    "num_disks",
    "seed",
    "num_reads",
    "num_stripes",
    "zipf_s",
    "policy",
    "timeout_ms",
    "window_id",
    "window_start_read",
    "window_end_read",
    "slow_disk_a_id",
    "slow_disk_a_state",
    "slow_disk_b_id",
    "slow_disk_b_state",
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
    "num_disks",
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

T2_SCHEDULE = [
    (0, 8, "healthy", 0, 3, "warmup_before_first_onset"),
    (1, 8, "mild_slow", 3, 6, "first_gradual_degradation"),
    (2, 8, "severe_slow", 6, 9, "sustained_severe_period"),
    (3, 8, "recovery", 9, 11, "partial_recovery"),
    (4, 8, "healthy", 11, 14, "recovered_interval"),
    (5, 8, "mild_slow", 14, 15, "relapse"),
    (6, 8, "recovery", 15, 16, "relapse_recovery"),
    (7, 8, "healthy", 16, 20, "post_recovery_observation"),
    (8, 9, "healthy", 0, 9, "staggered_later_onset"),
    (9, 9, "mild_slow", 9, 11, "second_disk_mild_period"),
    (10, 9, "severe_slow", 11, 14, "second_disk_severe_period"),
    (11, 9, "recovery", 14, 16, "second_disk_recovery"),
    (12, 9, "healthy", 16, 20, "second_disk_post_recovery"),
]

T3_SCHEDULE = [
    (event_id, 98 if disk == 8 else 99, state, start, end, notes)
    for event_id, disk, state, start, end, notes in T2_SCHEDULE
]


def make_t3_stress_schedule():
    schedule = []
    event_id = 0
    group_a_events = [
        ("healthy", 0, 3, "warmup_before_first_onset"),
        ("mild_slow", 3, 6, "first_gradual_degradation"),
        ("severe_slow", 6, 9, "sustained_severe_period"),
        ("recovery", 9, 11, "partial_recovery"),
        ("healthy", 11, 14, "recovered_interval"),
        ("mild_slow", 14, 15, "relapse"),
        ("recovery", 15, 16, "relapse_recovery"),
        ("healthy", 16, 20, "post_recovery_observation"),
    ]
    group_b_events = [
        ("healthy", 0, 9, "staggered_later_onset"),
        ("mild_slow", 9, 11, "second_disk_mild_period"),
        ("severe_slow", 11, 14, "second_disk_severe_period"),
        ("recovery", 14, 16, "second_disk_recovery"),
        ("healthy", 16, 20, "second_disk_post_recovery"),
    ]
    for disk in range(80, 90):
        for state, start, end, notes in group_a_events:
            schedule.append((event_id, disk, state, start, end, notes))
            event_id += 1
    for disk in range(90, 100):
        for state, start, end, notes in group_b_events:
            schedule.append((event_id, disk, state, start, end, notes))
            event_id += 1
    return schedule


T3_STRESS_SCHEDULE = make_t3_stress_schedule()


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


def state_for_disk_window(schedule, disk_id, window_id):
    for _, disk, state, start, end, _ in schedule:
        if disk == disk_id and start <= window_id < end:
            return state
    return "healthy"


def active_slow_disks(schedule, window_id):
    return sum(
        1
        for _, disk, state, start, end, _ in schedule
        if start <= window_id < end and state in ("mild_slow", "severe_slow")
    )


def check_numeric_fields(row, fields):
    for field in fields:
        try:
            float(row[field])
        except ValueError as exc:
            fail(f"expected numeric field {field}: {row[field]} ({exc})")


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
    for row in rows:
        if row["scenario"] != "dynamic_degradation":
            fail(f"unexpected aggregate scenario: {row}")
        if int(row["num_disks"]) != 10:
            fail(f"unexpected aggregate num_disks: {row}")


def check_extended_aggregate(runner):
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
        "--extended-metrics",
    ])
    rows = csv_rows(result.stdout, EXTENDED_DYNAMIC_HEADER)
    if len(rows) != len(POLICIES):
        fail(f"expected 4 extended aggregate rows, got {len(rows)}")
    by_policy = {row["policy"]: row for row in rows}
    for policy in POLICIES:
        row = by_policy[policy]
        if int(row["num_disks"]) != 10:
            fail(f"unexpected extended num_disks: {row}")
        if float(row["post_warmup_windowed_p99_ms"]) < 0.0:
            fail(f"invalid post-warmup p99 for {policy}: {row}")
        if float(row["severe_window_p99_ms"]) < 0.0:
            fail(f"invalid severe-window p99 for {policy}: {row}")
        if policy == "health_ec":
            eligible = int(row["read_bypass_eligible_events"])
            triggered = int(row["read_bypass_triggered_events"])
            if eligible < 0 or triggered < 0 or triggered > eligible:
                fail(f"invalid Health-EC read bypass counts: {row}")
            coverage = float(row["read_bypass_coverage_pct"])
            if eligible == 0 and coverage != -1.0:
                fail(f"expected -1 coverage for zero eligibility: {row}")
            if eligible > 0 and not (0.0 <= coverage <= 100.0):
                fail(f"invalid Health-EC read bypass coverage: {row}")
        else:
            for field in [
                "read_bypass_eligible_events",
                "read_bypass_triggered_events",
                "read_bypass_coverage_pct",
            ]:
                if row[field] != "-1.0" and row[field] != "-1":
                    fail(f"expected N/A read-bypass field for {policy}: {row}")


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
    for row in rows:
        window_id = int(row["window_id"])
        if int(row["num_disks"]) != 10:
            fail(f"unexpected windowed num_disks: {row}")
        if int(row["slow_disk_a_id"]) != 8 or int(row["slow_disk_b_id"]) != 9:
            fail(f"unexpected windowed slow disk ids: {row}")
        if row["slow_disk_a_state"] != state_for_disk_window(T2_SCHEDULE, 8, window_id):
            fail(f"unexpected slow_disk_a_state: {row}")
        if row["slow_disk_b_state"] != state_for_disk_window(T2_SCHEDULE, 9, window_id):
            fail(f"unexpected slow_disk_b_state: {row}")
        if int(row["active_slow_disks"]) != active_slow_disks(T2_SCHEDULE, window_id):
            fail(f"unexpected active_slow_disks: {row}")


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
    for row, expected in zip(rows, T2_SCHEDULE):
        event_id, disk_id, state, start_window, end_window, notes = expected
        if row["scenario"] != "dynamic_degradation":
            fail(f"unexpected T2 event scenario: {row}")
        if int(row["num_disks"]) != 10:
            fail(f"unexpected T2 event num_disks: {row}")
        if int(row["event_id"]) != event_id or int(row["disk_id"]) != disk_id:
            fail(f"unexpected T2 event id/disk: {row}")
        if row["state"] != state or row["notes"] != notes:
            fail(f"unexpected T2 event state/notes: {row}")
        if int(row["start_window"]) != start_window or int(row["end_window"]) != end_window:
            fail(f"unexpected T2 event windows: {row}")


def check_t3_lightweight(runner):
    result = run([
        runner,
        "--scenario",
        "dynamic_realistic_100d_2pct_hdd",
        "--num-reads",
        "20000",
        "--num-stripes",
        "5000",
        "--policy",
        "all",
        "--format",
        "csv",
        "--extended-metrics",
    ])
    rows = csv_rows(result.stdout, EXTENDED_DYNAMIC_HEADER)
    if len(rows) != len(POLICIES):
        fail(f"expected 4 T3 aggregate rows, got {len(rows)}")
    order = [row["policy"] for row in rows]
    if order != POLICIES:
        fail(f"unexpected T3 policy order: {order}")
    for row in rows:
        if row["scenario"] != "dynamic_realistic_100d_2pct_hdd":
            fail(f"unexpected T3 scenario: {row}")
        if int(row["num_disks"]) != 100:
            fail(f"unexpected T3 num_disks: {row}")
        if int(row["num_reads"]) != 20000:
            fail(f"unexpected T3 num_reads: {row}")
        if int(row["num_stripes"]) != 5000:
            fail(f"unexpected T3 num_stripes: {row}")
        if int(row["num_windows"]) != 20 or int(row["window_size"]) != 1000:
            fail(f"unexpected T3 window shape: {row}")
        check_numeric_fields(row, [
            "p50_ms",
            "p95_ms",
            "p99_ms",
            "post_warmup_windowed_p99_ms",
            "severe_window_p99_ms",
            "bandwidth_overhead_pct",
        ])

    result = run([
        runner,
        "--scenario",
        "dynamic_realistic_100d_2pct_hdd",
        "--num-reads",
        "20000",
        "--num-stripes",
        "5000",
        "--policy",
        "all",
        "--format",
        "windowed_csv",
    ])
    rows = csv_rows(result.stdout, WINDOWED_HEADER)
    if len(rows) != len(POLICIES) * 20:
        fail(f"expected 80 T3 windowed rows, got {len(rows)}")
    policy_order = []
    for row in rows:
        if not policy_order or policy_order[-1] != row["policy"]:
            policy_order.append(row["policy"])
    if policy_order != POLICIES:
        fail(f"unexpected T3 windowed policy order: {policy_order}")
    for row in rows:
        window_id = int(row["window_id"])
        if row["scenario"] != "dynamic_realistic_100d_2pct_hdd":
            fail(f"unexpected T3 windowed scenario: {row}")
        if int(row["num_disks"]) != 100:
            fail(f"unexpected T3 windowed num_disks: {row}")
        if int(row["slow_disk_a_id"]) != 98 or int(row["slow_disk_b_id"]) != 99:
            fail(f"unexpected T3 slow disk ids: {row}")
        if row["slow_disk_a_state"] != state_for_disk_window(T3_SCHEDULE, 98, window_id):
            fail(f"unexpected T3 slow_disk_a_state: {row}")
        if row["slow_disk_b_state"] != state_for_disk_window(T3_SCHEDULE, 99, window_id):
            fail(f"unexpected T3 slow_disk_b_state: {row}")
        if int(row["active_slow_disks"]) != active_slow_disks(T3_SCHEDULE, window_id):
            fail(f"unexpected T3 active_slow_disks: {row}")


def check_t3_event_trace_contract(runner):
    result = run([
        runner,
        "--scenario",
        "dynamic_realistic_100d_2pct_hdd",
        "--seed",
        "42",
        "--num-reads",
        "1000000",
        "--num-stripes",
        "5000",
        "--format",
        "event_trace",
    ])
    rows = csv_rows(result.stdout, EVENT_TRACE_HEADER)
    if len(rows) != len(T3_SCHEDULE):
        fail(f"expected 13 T3 event trace rows, got {len(rows)}")
    window_size = 50000
    for row, expected in zip(rows, T3_SCHEDULE):
        event_id, disk_id, state, start_window, end_window, notes = expected
        expected_positive = 1 if state in ("mild_slow", "severe_slow") else 0
        expected_values = {
            "scenario": "dynamic_realistic_100d_2pct_hdd",
            "num_disks": "100",
            "seed": "42",
            "event_id": str(event_id),
            "disk_id": str(disk_id),
            "state": state,
            "start_window": str(start_window),
            "end_window": str(end_window),
            "start_read": str(start_window * window_size),
            "end_read": str(end_window * window_size),
            "is_migration_positive": str(expected_positive),
            "notes": notes,
        }
        for field, value in expected_values.items():
            if row[field] != value:
                fail(f"unexpected T3 event trace {field}: expected {value}, got {row[field]}")


def check_t3_stress_lightweight(runner):
    scenario = "dynamic_stress_100d_20pct_hdd"
    result = run([
        runner,
        "--scenario",
        scenario,
        "--num-reads",
        "20000",
        "--num-stripes",
        "5000",
        "--policy",
        "all",
        "--format",
        "csv",
        "--extended-metrics",
    ])
    rows = csv_rows(result.stdout, EXTENDED_DYNAMIC_HEADER)
    if len(rows) != len(POLICIES):
        fail(f"expected 4 T3.2 aggregate rows, got {len(rows)}")
    order = [row["policy"] for row in rows]
    if order != POLICIES:
        fail(f"unexpected T3.2 policy order: {order}")
    for row in rows:
        if row["scenario"] != scenario:
            fail(f"unexpected T3.2 scenario: {row}")
        if int(row["num_disks"]) != 100:
            fail(f"unexpected T3.2 num_disks: {row}")
        if int(row["num_reads"]) != 20000:
            fail(f"unexpected T3.2 num_reads: {row}")
        if int(row["num_stripes"]) != 5000:
            fail(f"unexpected T3.2 num_stripes: {row}")
        if int(row["num_windows"]) != 20 or int(row["window_size"]) != 1000:
            fail(f"unexpected T3.2 window shape: {row}")
        check_numeric_fields(row, [
            "p50_ms",
            "p95_ms",
            "p99_ms",
            "post_warmup_windowed_p99_ms",
            "severe_window_p99_ms",
            "bandwidth_overhead_pct",
        ])

    result = run([
        runner,
        "--scenario",
        scenario,
        "--num-reads",
        "20000",
        "--num-stripes",
        "5000",
        "--policy",
        "all",
        "--format",
        "windowed_csv",
    ])
    rows = csv_rows(result.stdout, WINDOWED_HEADER)
    if len(rows) != len(POLICIES) * 20:
        fail(f"expected 80 T3.2 windowed rows, got {len(rows)}")
    policy_order = []
    for row in rows:
        if not policy_order or policy_order[-1] != row["policy"]:
            policy_order.append(row["policy"])
    if policy_order != POLICIES:
        fail(f"unexpected T3.2 windowed policy order: {policy_order}")
    for row in rows:
        window_id = int(row["window_id"])
        if row["scenario"] != scenario:
            fail(f"unexpected T3.2 windowed scenario: {row}")
        if int(row["num_disks"]) != 100:
            fail(f"unexpected T3.2 windowed num_disks: {row}")
        if int(row["slow_disk_a_id"]) != 80 or int(row["slow_disk_b_id"]) != 90:
            fail(f"unexpected T3.2 slow disk representatives: {row}")
        if row["slow_disk_a_state"] != state_for_disk_window(T3_STRESS_SCHEDULE, 80, window_id):
            fail(f"unexpected T3.2 slow_disk_a_state: {row}")
        if row["slow_disk_b_state"] != state_for_disk_window(T3_STRESS_SCHEDULE, 90, window_id):
            fail(f"unexpected T3.2 slow_disk_b_state: {row}")
        if int(row["active_slow_disks"]) != active_slow_disks(T3_STRESS_SCHEDULE, window_id):
            fail(f"unexpected T3.2 active_slow_disks: {row}")


def check_t3_stress_event_trace_contract(runner):
    scenario = "dynamic_stress_100d_20pct_hdd"
    result = run([
        runner,
        "--scenario",
        scenario,
        "--seed",
        "42",
        "--num-reads",
        "1000000",
        "--num-stripes",
        "5000",
        "--format",
        "event_trace",
    ])
    rows = csv_rows(result.stdout, EVENT_TRACE_HEADER)
    if len(rows) != len(T3_STRESS_SCHEDULE):
        fail(f"expected 130 T3.2 event trace rows, got {len(rows)}")
    if {int(row["disk_id"]) for row in rows} != set(range(80, 100)):
        fail("unexpected T3.2 event trace disk set")
    window_size = 50000
    for row, expected in zip(rows, T3_STRESS_SCHEDULE):
        event_id, disk_id, state, start_window, end_window, notes = expected
        expected_positive = 1 if state in ("mild_slow", "severe_slow") else 0
        expected_values = {
            "scenario": scenario,
            "num_disks": "100",
            "seed": "42",
            "event_id": str(event_id),
            "disk_id": str(disk_id),
            "state": state,
            "start_window": str(start_window),
            "end_window": str(end_window),
            "start_read": str(start_window * window_size),
            "end_read": str(end_window * window_size),
            "is_migration_positive": str(expected_positive),
            "notes": notes,
        }
        for field, value in expected_values.items():
            if row[field] != value:
                fail(
                    f"unexpected T3.2 event trace {field}: "
                    f"expected {value}, got {row[field]}"
                )


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
    run(
        [
            runner,
            "--scenario",
            "canonical_stress20",
            "--format",
            "csv",
            "--extended-metrics",
        ],
        expect_success=False,
        expected_stderr="extended metrics require",
    )
    run(
        [
            runner,
            "--scenario",
            "dynamic_degradation",
            "--num-reads",
            "20000",
            "--format",
            "windowed_csv",
            "--extended-metrics",
        ],
        expect_success=False,
        expected_stderr="extended metrics require",
    )
    run(
        [
            runner,
            "--scenario",
            "not_a_scenario",
            "--format",
            "csv",
        ],
        expect_success=False,
        expected_stderr="invalid --scenario",
    )
    run(
        [
            runner,
            "--scenario",
            "canonical_stress20",
            "--format",
            "windowed_csv",
        ],
        expect_success=False,
        expected_stderr="require a dynamic scenario",
    )


def main():
    if len(sys.argv) != 2:
        fail("usage: validate_compare_policies_contract.py <compare_policies>")
    runner = sys.argv[1]
    check_aggregate(runner)
    check_extended_aggregate(runner)
    check_windowed(runner)
    check_event_trace(runner)
    check_t3_lightweight(runner)
    check_t3_event_trace_contract(runner)
    check_t3_stress_lightweight(runner)
    check_t3_stress_event_trace_contract(runner)
    check_negative_commands(runner)


if __name__ == "__main__":
    main()
