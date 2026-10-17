if not __debug__:
    raise RuntimeError("contract tests require assertions enabled")
"""Check passive instrumentation, accounting, D bound and utility identities."""
import csv
import io
import math
import subprocess
import sys


def run(runner, args, success=True):
    p = subprocess.run([runner, *args], capture_output=True, text=True)
    assert (p.returncode == 0) == success, p.stderr
    return p.stdout


def main():
    runner = sys.argv[1]
    for scene in ("dynamic_degradation", "dynamic_realistic_100d_2pct_hdd",
                  "dynamic_overlap_100d_4pct_hdd", "dynamic_stress_100d_20pct_hdd"):
        for strategy in ("default", "disabled"):
            args = ["--scenario", scene, "--num-reads", "20000", "--num-stripes", "500",
                    "--seed", "91001", "--policy", "health_ec", "--health-migration-strategy", strategy]
            before = run(runner, args + ["--format", "csv"])
            after = run(runner, args + ["--format", "csv", "--observe-shard-diagnostics"])
            assert before == after, "instrumentation changed policy output"
            aggregate = next(csv.DictReader(io.StringIO(before)))
            rows = list(csv.DictReader(io.StringIO(run(runner, args + ["--format", "shard_diagnostic"]))))
            assert len({(r["window_id"], r["shard_id"], r["disk_id"]) for r in rows}) == len(rows)
            from contract_support import validate_shard_rows
            disks=10 if scene=='dynamic_degradation' else 100
            moves=list(csv.DictReader(io.StringIO(run(runner,args+["--format","migration_trace"]))))
            current={sh:([6,7,8,9,4,5][sh] if scene=='dynamic_degradation' and sh<6 else sh%disks)
                     for sh in range(3000)}
            possible={}
            for w in range(20):
                for sh in current:possible[w,sh]={current[sh]}
                for move in (x for x in moves if int(x['window_id'])==w):
                    sh=int(move['shard_id']);src=int(move['source_disk']);dst=int(move['target_disk'])
                    assert current[sh]==src and src!=dst
                    possible[w,sh].add(dst);current[sh]=dst
            validate_shard_rows(rows,scenario=scene,seed=91001,policy='health_ec',stripes=500,
                                disks=disks,windows=20,source_for=lambda w,sh:possible[w,sh])
            assert sum(int(r["reads"]) for r in rows) == 80000
            assert sum(int(r["migrations"]) for r in rows) == int(aggregate["migration_triggers"])
            assert sum(int(r["proactive_reads"]) for r in rows) == 4 * int(aggregate["parity_reads"])
            for r in rows:
                reads, active, wins, targets = (int(r[k]) for k in
                    ("reads", "proactive_reads", "parity_wins", "utility_target_reads"))
                assert 0 <= wins <= active <= reads and 0 <= targets <= reads
                assert int(r["utility_positive_reads"]) + int(r["utility_negative_reads"]) <= targets
                assert 0 <= int(r["target_missing_at_threshold"]) <= int(r["d_above_updates"]) <= active
                assert float(r["max_d"]) <= float(r["hotness"]) + 1e-8
                assert int(r["d_reachable"]) == (float(r["hotness"]) > 12)
                if not int(r["d_reachable"]):
                    assert int(r["d_above_updates"]) == 0
                for key in ("latency_gain_sum_ms", "max_s", "max_d"):
                    assert math.isfinite(float(r[key]))
                for threshold in (15, 30):
                    assert 0 <= int(r[f"tail{threshold}_removed"]) <= int(r["utility_positive_reads"])
                    assert 0 <= int(r[f"tail{threshold}_added"]) <= int(r["utility_negative_reads"])
    run(runner, ["--format", "shard_diagnostic"], False)
    run(runner, ["--scenario", "dynamic_degradation", "--format", "shard_diagnostic", "--policy", "vanilla_ec"], False)
    run(runner, ["--scenario", "dynamic_degradation", "--format", "shard_diagnostic", "--policy", "health_ec",
                 "--health-migration-strategy", "topk_budgeted"], False)
    print("shard diagnostic accounting / D bound / passive policy equality PASS")


if __name__ == "__main__":
    main()
