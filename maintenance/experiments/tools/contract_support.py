"""Standalone maintained output contracts (stdlib only)."""
from collections import defaultdict
import csv
import io
import math
class ContractError(ValueError):
    pass

def require(condition, message):
    if not condition:
        raise ContractError(message)

def finite(value, *, minimum=None, maximum=None):
    require(not isinstance(value,bool), 'boolean is not a number')
    try:
        value=float(value)
    except (TypeError,ValueError) as exc:
        raise ContractError('expected numeric value') from exc
    require(math.isfinite(value),'nonfinite value')
    require(minimum is None or value>=minimum,'below minimum')
    require(maximum is None or value<=maximum,'above maximum')
    return value

def integer(value, *, minimum=0, maximum=None):
    require(not isinstance(value,bool),'boolean is not a count')
    text=str(value)
    require(text.isascii() and (text.isdigit() or (text.startswith('-') and text[1:].isdigit())),
            'expected exact integer')
    value=int(text)
    require(minimum is None or value>=minimum,'count below minimum')
    require(maximum is None or value<=maximum,'count above maximum')
    return value

def parse_csv(data, header, *, allow_empty=False):
    text=data.decode('utf-8') if isinstance(data,bytes) else data
    reader=csv.reader(io.StringIO(text),strict=True)
    try:
        actual=next(reader)
    except StopIteration as exc:
        raise ContractError('missing CSV header') from exc
    require(actual==list(header) and len(set(actual))==len(actual),'CSV header mismatch/duplicates')
    result=[]
    for row in reader:
        require(len(row)==len(actual),'truncated or extra CSV columns')
        result.append(dict(zip(actual,row,strict=True)))
    require(allow_empty or result,'missing CSV data')
    return result

def exact_keys(rows, fields, expected):
    actual=[tuple(r[f] for f in fields) for r in rows]
    expected=list(expected)
    require(len(set(expected))==len(expected),'duplicate expected key')
    require(len(set(actual))==len(actual),'duplicate actual key')
    require(set(actual)==set(expected),'missing, extra or wrong identity key')
    return {key:row for key,row in zip(actual,rows,strict=True)}

def ratio(numerator, denominator):
    numerator=finite(numerator,minimum=0);denominator=finite(denominator,minimum=0)
    return numerator/denominator if denominator else None

def validate_window(row, *, windows, total_reads):
    w=integer(row['window_id'],maximum=windows-1)
    require(total_reads%windows==0,'nonintegral schedule window')
    width=total_reads//windows
    require(integer(row['window_start_read'])==w*width,'window start mismatch')
    require(integer(row['window_end_read'])==(w+1)*width,'window end mismatch')

def validate_disk_rows(rows, *, scenario,seed,policies,disks,reads,bin_reads):
    require(reads%bin_reads==0,'bin must divide workload')
    bins=reads//bin_reads
    expected=[(p,str(d),str(w)) for p in policies for w in range(bins) for d in range(disks)]
    exact_keys(rows,('policy','disk_id','detector_window'),expected)
    previous={}
    for r in rows:
        require(r['scenario']==scenario and integer(r['seed'])==seed,'trace identity mismatch')
        w=integer(r['detector_window'],maximum=bins-1)
        key=(r['policy'],integer(r['disk_id'],maximum=disks-1))
        require(w>previous.get(key,-1),'disk windows out of order')
        previous[key]=w
        require(integer(r['window_start_read'])==w*bin_reads and
                integer(r['window_end_read'])==(w+1)*bin_reads,'detector bin boundaries mismatch')
        count=integer(r['read_count'])
        require(count==integer(r['data_read_count'])+integer(r['parity_read_count']),'read accounting')
        require(integer(r['read_bytes'])==4096*count,'read bytes mismatch')
        require(finite(r['pressure_proxy'],minimum=0)==count,'pressure mismatch')
        if count:
            finite(r['observed_latency_ms'],minimum=0)
        else:
            require(r['observed_latency_ms']=='','empty disk must have missing latency')
        require(integer(r['validation_bypass_triggered'])<=integer(r['validation_bypass_eligible']),
                'bypass accounting')
    return rows

def validate_shard_rows(rows, *, scenario,seed,policy,stripes,disks,windows,source_for=None):
    keys=[]
    for r in rows:
        require(r['scenario']==scenario and integer(r['seed'])==seed and r['policy']==policy,
                'shard output identity mismatch')
        window=integer(r['window_id'],maximum=windows-1)
        shard=integer(r['shard_id'],maximum=stripes*6-1)
        disk=integer(r['disk_id'],maximum=disks-1)
        if 'stripe_id' in r:
            require(integer(r['stripe_id'])==shard//6,'shard/stripe mismatch')
        if source_for is not None:
            expected=source_for(window,shard)
            require(disk in expected if isinstance(expected,(set,list,tuple)) else disk==expected,
                    'policy-local source mismatch')
        keys.append((window,shard,disk))
        n,active,wins,target=(integer(r[k]) for k in ('reads','proactive_reads','parity_wins','utility_target_reads'))
        require(0<=wins<=active<=n and target<=n,'invalid shard read counts')
        positive=integer(r['utility_positive_reads']);negative=integer(r['utility_negative_reads'])
        require(positive+negative<=target,'invalid signed gain counts')
        gain=finite(r['latency_gain_sum_ms'])
        require(not (positive==negative==0 and abs(gain)>1e-8),'gain without signed observations')
        require(not (positive==0 and gain>1e-8),'positive gain without positive observations')
        require(not (negative==0 and gain<-1e-8),'negative gain without negative observations')
        for key in ('max_s','max_d','hotness'):
            if key in r:finite(r[key],minimum=0)
        for threshold in (15,30):
            if f'tail{threshold}_removed' in r:
                require(integer(r[f'tail{threshold}_removed'])<=positive,'removed tail mismatch')
                require(integer(r[f'tail{threshold}_added'])<=negative,'added tail mismatch')
    require(len(keys)==len(set(keys)),'duplicate shard observation')
    return rows

def validate_migrations(rows, *, scenario,seed,reads,windows,stripes,disks,source_for=None):
    for r in rows:
        require(r['scenario']==scenario and integer(r['seed'])==seed,'migration identity mismatch')
        shard=integer(r['shard_id'],maximum=6*stripes-1)
        require(integer(r['stripe_id'])==shard//6,'migration stripe/shard mismatch')
        index=integer(r['read_index'],maximum=reads-1)
        window=integer(r['window_id'],maximum=windows-1)
        require(index//(reads//windows)==window,'migration action time mismatch')
        src=integer(r['source_disk'],maximum=disks-1);dst=integer(r['target_disk'],maximum=disks-1)
        require(src!=dst,'migration must change disk')
        if source_for is not None:require(src==source_for(window,shard),'migration source mismatch')
    return rows

def validate_public_csv(rows, *, num_reads,scenario=None,seed=None):
    sentinels={'migration_true_positives','migration_false_positives','migration_false_negatives',
               'first_detection_latency_reads','first_mitigation_latency_reads','recovery_regret_reads'}
    groups=defaultdict(list)
    for r in rows:
        require(None not in r and all(v is not None for v in r.values()),'CSV row shape mismatch')
        if scenario is not None:require(r['scenario']==scenario,'scenario mismatch')
        if seed is not None:require(integer(r['seed'])==seed,'seed mismatch')
        if 'num_reads' in r:require(integer(r['num_reads'])==num_reads,'workload reads mismatch')
        for key,value in r.items():
            if key=='read_bypass_coverage_pct':
                continue  # Its -1 N/A value is checked with both counters below.
            if key.endswith('_ms') or key.endswith('_pct') or key in ('zipf_s','timeout_ms'):
                finite(value,minimum=None if key.endswith('improvement_pct') else 0)
            if key.endswith('_reads') or key.endswith('_count') or key in ('issued_shard_reads','parity_reads','migration_triggers'):
                integer(value,minimum=-1 if key in sentinels else 0)
        if 'window_id' in r and 'p99_ms' in r:
            validate_window(r,windows=20,total_reads=num_reads)
            groups[(r['scenario'],r['seed'],r['policy'])].append(integer(r['window_id']))
        if 'read_bypass_coverage_pct' in r:
            eligible=integer(r['read_bypass_eligible_events'],minimum=-1)
            triggered=integer(r['read_bypass_triggered_events'],minimum=-1)
            coverage=finite(r['read_bypass_coverage_pct'],minimum=-1,maximum=100)
            if r.get('policy') in ('vanilla_ec','late_binding','timeout_degraded_read'):
                require((eligible,triggered,coverage)==(-1,-1,-1),
                        'unsupported read-bypass metrics must all be N/A')
            else:
                require(0<=triggered<=eligible,'invalid read-bypass counters')
                expected=100*triggered/eligible if eligible else -1
                require(abs(coverage-expected)<=.051,'coverage/count formula mismatch')
        if 'start_window' in r:
            start=integer(r['start_window'],maximum=19);end=integer(r['end_window'],maximum=20)
            require(start<end and integer(r['start_read'])==start*(num_reads//20) and
                    integer(r['end_read'])==end*(num_reads//20),'event time mismatch')
            require(integer(r['is_migration_positive'],maximum=1)==int(r['state'] in ('mild_slow','severe_slow')),
                    'event truth mismatch')
    for ids in groups.values():require(ids==list(range(20)),'window set/order incomplete')
    if rows and 'source_disk' in rows[0] and 'read_index' in rows[0]:
        r=rows[0]
        validate_migrations(rows,scenario=r['scenario'],seed=integer(r['seed']),reads=num_reads,
            windows=20,stripes=integer(r['num_stripes']),disks=integer(r['num_disks']))
    return rows
