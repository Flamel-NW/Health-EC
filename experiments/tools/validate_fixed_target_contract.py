"""Validate passive target identity, chronology, and accounting without workflow."""
import csv
import io
import math
import subprocess
import sys


def run(runner, args, success=True):
    p=subprocess.run([runner,*args],capture_output=True,text=True)
    assert (p.returncode==0)==success,p.stderr
    return p.stdout


def validate_rows(rows, reads, num_disks):
    keys=set()
    size=reads//20
    for r in rows:
        past,future,budget,shard,source,target=(int(r[k]) for k in
            ("past_window","future_window","budget","shard_id","source_disk","target_disk"))
        key=(past,budget,shard,source)
        assert key not in keys
        keys.add(key)
        assert 0<=past<19 and future==past+1 and budget in (1,4,16)
        assert 0<=source<num_disks and 0<=target<num_disks and target!=source
        assert past*size<=int(r["target_chosen_read"])<int(r["decision_read"])==future*size
        assert int(r["past_reads"])==int(r["past_available_reads"])>0
        assert float(r["past_gain_ms"])>0
        n,a,m,c=(int(r[k]) for k in ("future_reads","available_reads","source_changed","target_conflict"))
        assert n==a+m+c and min(n,a,m,c)>=0
        assert 0<=int(r["negative_reads"])+int(r["zero_reads"])<=a
        for k in ("past_gain_ms","future_gain_ms"):
            assert math.isfinite(float(r[k]))
        for threshold in (15,30):
            assert 0<=int(r[f"tail{threshold}_added"])<=int(r["negative_reads"])
            assert 0<=int(r[f"tail{threshold}_removed"])<=a
    for past in range(19):
        for budget in (1,4,16):
            assert sum(int(r["past_window"])==past and int(r["budget"])==budget for r in rows)<=budget


def main():
    runner=sys.argv[1]
    for scene in ("dynamic_degradation","dynamic_realistic_100d_2pct_hdd",
                  "dynamic_overlap_100d_4pct_hdd","dynamic_stress_100d_20pct_hdd"):
        for strategy in ("default","disabled"):
            args=["--scenario",scene,"--num-reads","20000","--num-stripes","500","--seed","95021",
                  "--policy","health_ec","--health-migration-strategy",strategy]
            before=run(runner,args+["--format","csv"])
            after=run(runner,args+["--format","csv","--observe-fixed-targets"])
            assert before==after,"fixed-target observer changed policy"
            rows=list(csv.DictReader(io.StringIO(run(runner,args+["--format","fixed_target_trace"]))))
            assert rows,"empty diagnostic fixture"
            validate_rows(rows,20000,10 if scene=="dynamic_degradation" else 100)
            # It must be impossible for the optional observer to change disk signals.
            disk_args=["--format","disk_trace","--detector-bin-reads","1000"]
            assert run(runner,args+disk_args)==run(runner,args+disk_args+["--observe-fixed-targets"])
    run(runner,["--format","fixed_target_trace"],False)
    run(runner,["--scenario","dynamic_degradation","--policy","vanilla_ec","--format","fixed_target_trace"],False)
    run(runner,["--scenario","dynamic_degradation","--policy","health_ec","--health-migration-strategy","topk_budgeted","--observe-fixed-targets"],False)
    print("fixed target chronology / accounting / passive CSV+disk equality PASS")


if __name__=="__main__":
    main()
