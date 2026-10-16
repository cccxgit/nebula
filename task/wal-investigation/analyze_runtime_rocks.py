#!/usr/bin/env python3
"""Read existing runtime evidence and emit independently recomputable metrics.
No database requests or service/config changes. JSON fields retain each time
window instead of equating CSV, WAL-observer, and full-WAL-snapshot endpoints.
"""
import collections
import csv
import datetime as dt
import hashlib
import io
import json
from pathlib import Path
import re

HERE = Path(__file__).resolve().parent
METRICS = ["status_VmRSS_kib", "smaps_Rss_kib", "status_RssAnon_kib",
           "status_RssFile_kib", "status_VmSwap_kib", "status_Threads",
           "wal_records_estimate", "wal_bytes", "wal_payload_bytes_estimate",
           "rocksdb.cur-size-all-mem-tables", "rocksdb.size-all-mem-tables",
           "rocksdb.num-entries-active-mem-table", "rocksdb.num-entries-imm-mem-tables",
           "rocksdb.num-immutable-mem-table", "rocksdb.estimate-table-readers-mem",
           "rocksdb.block-cache-usage", "rocksdb.block-cache-pinned-usage",
           "rocksdb.block-cache-capacity"]


def stats(xs, ys):
    xm, ym = sum(xs) / len(xs), sum(ys) / len(ys)
    xx = sum((x-xm)**2 for x in xs)
    slope = sum((x-xm)*(y-ym) for x,y in zip(xs,ys)) / xx if xx else None
    yy = sum((y-ym)**2 for y in ys)
    r2 = 1 - sum((y - (ym + slope*(x-xm)))**2 for x,y in zip(xs,ys)) / yy if yy and slope is not None else None
    changes = [b-a for a,b in zip(ys,ys[1:])]
    return {"first":ys[0], "last":ys[-1], "delta":ys[-1]-ys[0],
            "min":min(ys), "max":max(ys), "ols_per_second":slope, "ols_r_squared":r2,
            "adjacent_increases":sum(x>0 for x in changes),
            "adjacent_decreases":sum(x<0 for x in changes), "adjacent_equal":sum(x==0 for x in changes)}


def read_observers(label):
    path=HERE/f"service-logs/{label}/nebula-storaged.INFO"
    local=dt.timezone(dt.timedelta(hours=8))
    result=[]
    for line in path.read_text().splitlines():
        if "WAL_MEMORY_OBSERVE " not in line: continue
        match=re.match(r"I(\d{8}) (\d{2}:\d{2}:\d{2}\.\d+)",line)
        if not match: continue
        timestamp=dt.datetime.strptime(" ".join(match.groups()),"%Y%m%d %H:%M:%S.%f").replace(tzinfo=local).astimezone(dt.timezone.utc)
        values={k:int(v) for k,v in re.findall(r"(\w+)=(-?\d+)(?= |$)",line)}
        result.append({"time_utc":timestamp.isoformat(),"epoch":timestamp.timestamp(),**values})
    return result


def main():
    events=[json.loads(line) for line in (HERE/"events.jsonl").read_text().splitlines()]
    completed={r['phase'] for r in events if r.get('action')=='phase_end'}
    observers={label:read_observers(label) for label in ('s1','s2','s3')}
    output={"generated_utc":dt.datetime.now(dt.timezone.utc).isoformat(),
            "primary_rss_field":"status_VmRSS_kib; smaps_Rss_kib also retained",
            "units":"proc memory KiB, RocksDB/WAL memory bytes, counts unitless",
            "validation":"50 spaces and engines, no HTTP/WAL errors, 1000 WAL part dirs, no errors text",
            "phases":{}, "notes":["No fitting across process restarts.","Zero-size fields are not inferred from missing samples.",
            "RocksDB memtable size is approximate arena allocation, not exact resident/live payload bytes.",
            "WAL observer and full-file snapshot windows are independent from CSV windows."]}
    for phase in sorted(p for p in HERE.glob('phase-*') if p.is_dir()):
        phase_result={"completed":phase.name in completed,"instances":{}}
        for path in sorted(phase.glob('s*.csv')):
            blob=path.read_bytes();rows=list(csv.DictReader(io.StringIO(blob.decode())))
            valid=[];invalid=[]
            for row in rows:
                ok=all(row.get(k)==v for k,v in [('http_spaces_ok','50'),('http_spaces_failed','0'),('http_engines','50'),('wal_part_dirs','1000'),('wal_parse_errors','0'),('errors','')])
                (valid if ok else invalid).append(row)
            result={"source":str(path),"sha256":hashlib.sha256(blob).hexdigest(),
                    "rows":len(rows),"valid_rows":len(valid),"invalid_sample_indices":[r.get('sample') for r in invalid]}
            phase_result['instances'][path.stem]=result
            if not valid: continue
            identities=sorted({(r['pid'],r['proc_starttime_ticks']) for r in valid})
            result['process_identities']=identities
            if len(identities)!=1:
                result['fit_error']='Process restart: refusing combined fit';continue
            first,last=valid[0],valid[-1]
            xs=[float(r['elapsed_seconds']) for r in valid]
            result.update(first_time_utc=first['time_utc'],last_time_utc=last['time_utc'],duration_seconds=xs[-1]-xs[0])
            result['metrics']={key:stats(xs,[float(r[key]) for r in valid]) for key in METRICS}
            result['sample_cost_seconds_max']=max(float(r['sample_cost_seconds']) for r in valid)
            result['rss_status_minus_smaps_kib']={'min':min(float(r['status_VmRSS_kib'])-float(r['smaps_Rss_kib']) for r in valid),
                                                'max':max(float(r['status_VmRSS_kib'])-float(r['smaps_Rss_kib']) for r in valid)}
            lo=dt.datetime.fromisoformat(first['time_utc']).timestamp();hi=dt.datetime.fromisoformat(last['time_utc']).timestamp()
            selected=[r for r in observers[path.stem] if lo<=r['epoch']<=hi]
            if selected:
                dx=[r['epoch']-selected[0]['epoch'] for r in selected]
                result['wal_observer_within_csv']={'rows':len(selected),'first_time_utc':selected[0]['time_utc'],
                    'last_time_utc':selected[-1]['time_utc'],'duration_seconds':dx[-1],
                    'metrics':{key:stats(dx,[r[key] for r in selected]) for key in
                               ['pushes','empty_pushes','live_nodes','node_object_bytes','live_logical_bytes','freed','gc_runs','resets']}}
            if len(valid)>=10:
                half=len(valid)//2
                result['rss_halves_ols_kib_per_second']=[stats([float(r['elapsed_seconds']) for r in part],
                    [float(r['status_VmRSS_kib']) for r in part])['ols_per_second'] for part in [valid[:half],valid[half:]]]
        raw=phase/'raw-samples.jsonl'
        if raw.exists():
            by={}
            for line in raw.read_text().splitlines():
                try:r=json.loads(line)
                except json.JSONDecodeError:continue
                if all('values' in h for h in r['http']) and r['http']:by[r['label']]=r
            phase_result['last_saved_raw_memtable_distributions']={label:{'sample':r['sample'],'time_utc':r['time_utc'],
                'space_engine_counts_by_bytes':dict(collections.Counter(str(v) for h in r['http'] for v in h['values']['rocksdb.cur-size-all-mem-tables'].values()))}
                for label,r in by.items()}
        output['phases'][phase.name]=phase_result
    before_phase=output['phases'].get('phase-08-beforeflush',{})
    after_phase=output['phases'].get('phase-09-afterflush',{})
    if before_phase and after_phase:
        comparison={'after_phase_completed':after_phase['completed'],'instances':{},
                    'note':'Compare the explicit pre-flush sample with the latest after-flush sample; phase09 first sample may still include jobs in progress.'}
        verification=HERE/'flush-job-verification.json'
        if verification.exists():comparison['job_verification']=json.loads(verification.read_text())
        for label in ('s1','s2','s3'):
            a=before_phase['instances'][label];b=after_phase['instances'][label]
            if 'metrics' not in a or 'metrics' not in b:continue
            result={'before_time_utc':a['last_time_utc'],'after_time_utc':b['last_time_utc'],
                    'process_identities_match':a['process_identities']==b['process_identities'],
                    'metrics':{k:{'before':a['metrics'][k]['last'],'after':b['metrics'][k]['last'],
                        'delta':b['metrics'][k]['last']-a['metrics'][k]['last']} for k in METRICS}}
            t0=dt.datetime.fromisoformat(a['last_time_utc']).timestamp();t1=dt.datetime.fromisoformat(b['last_time_utc']).timestamp()
            result['duration_seconds']=t1-t0
            d0=[r for r in observers[label] if r['epoch']<=t0];d1=[r for r in observers[label] if r['epoch']<=t1]
            if d0 and d1:
                x,y=d0[-1],d1[-1]
                result['closest_preceding_observers']={'before_time_utc':x['time_utc'],'after_time_utc':y['time_utc'],
                    'before_offset_seconds':t0-x['epoch'],'after_offset_seconds':t1-y['epoch'],
                    'metrics':{k:{'before':x[k],'after':y[k],'delta':y[k]-x[k]} for k in
                               ['pushes','empty_pushes','live_nodes','node_object_bytes','freed','gc_runs','resets']}}
            after_rows=list(csv.DictReader((HERE/'phase-09-afterflush'/f'{label}.csv').open()))
            selected_points={}
            for point_name,index in [('first_in_progress',0),('memtable_trough_10s',1),('refill_20s',2),('stable_30s',3),('final',len(after_rows)-1)]:
                if index>=len(after_rows):continue
                row=after_rows[index]
                selected_points[point_name]={'sample':int(row['sample']),'time_utc':row['time_utc'],
                    'elapsed_seconds':float(row['elapsed_seconds']),
                    'metrics':{k:{'before':a['metrics'][k]['last'],'after':float(row[k]),
                        'delta':float(row[k])-a['metrics'][k]['last']} for k in METRICS}}
            result['selected_after_points']=selected_points
            comparison['instances'][label]=result
        output['flush_comparison']=comparison
    start=HERE/'wal-accelerated-start.json';end=HERE/'wal-accelerated-end.json'
    if start.exists() and end.exists():
        a=json.loads(start.read_text());b=json.loads(end.read_text())
        key=lambda r:(r['label'],r['space_id'],r['partition_id'],Path(r['path']).name)
        amap={key(r):r for r in a['files']};bmap={key(r):r for r in b['files']}
        common=sorted(amap.keys()&bmap.keys());changed=[];foreign=[]
        for k in common:
            x,y=amap[k],bmap[k]
            if x['last_term']!=y['last_term']:changed.append(k)
            increments={t:y['term_counts'].get(t,0)-x['term_counts'].get(t,0) for t in set(x['term_counts'])|set(y['term_counts'])}
            if any(n for t,n in increments.items() if int(t)!=x['last_term']):foreign.append({'key':k,'increments':increments})
        output['full_wal_snapshot_comparison']={'start_snapshot_window':[a['started_utc'],a['finished_utc']],
            'end_snapshot_window':[b['started_utc'],b['finished_utc']], 'common_files':len(common),
            'missing_at_end':len(amap.keys()-bmap.keys()),'new_at_end':len(bmap.keys()-amap.keys()),
            'last_term_changed_count':len(changed),'last_term_changed_examples':changed[:10],
            'files_with_added_records_outside_start_last_term':len(foreign),'foreign_term_examples':foreign[:10],
            'start_total':a['total'],'end_total':b['total'],'records_added':b['total']['records']-a['total']['records'],
            'has_anomalies':[a['has_anomalies'],b['has_anomalies']]}
    target=HERE/'runtime-metrics.json';target.write_text(json.dumps(output,indent=2,ensure_ascii=False)+'\n')
    print(str(target))
    for name,phase in output['phases'].items():
        for label,r in phase['instances'].items():
            if 'metrics' in r:
                m=r['metrics'];print(name,label,r['valid_rows'],r['duration_seconds'],
                    'RSS_VmRSS_delta_KiB',m['status_VmRSS_kib']['delta'],'Rocks_delta_MiB',m['rocksdb.cur-size-all-mem-tables']['delta']/1048576,
                    'WAL_per_sec',m['wal_records_estimate']['ols_per_second'])
    if 'full_wal_snapshot_comparison' in output:
        s=output['full_wal_snapshot_comparison'];print('Snapshot terms:',s['common_files'],s['last_term_changed_count'],s['files_with_added_records_outside_start_last_term'])


if __name__=='__main__':main()
