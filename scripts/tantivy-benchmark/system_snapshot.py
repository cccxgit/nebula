#!/usr/bin/env python3
"""Snapshot allocated graph/index blocks and service/system memory between stages.

Only walks metadata (st_blocks); it does not read file contents, flush caches,
stop services, or claim an instantaneous/peak measurement. Do not run during a
query latency measurement. Missing explicitly named index directories count zero.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import stat
import time

from monitor import read_process


def stamp():
    return time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())


def allocated(path):
    start = time.monotonic()
    if not path.exists():
        return {'exists': False, 'allocated_bytes': 0, 'entries': 0, 'elapsed_s': 0}
    seen, total, count, stack = set(), 0, 0, [path]
    while stack:
        entry = stack.pop()
        info = entry.lstat()
        key = (info.st_dev, info.st_ino)
        if key in seen:
            continue
        seen.add(key)
        total += info.st_blocks * 512
        count += 1
        if stat.S_ISDIR(info.st_mode):
            with os.scandir(entry) as children:
                stack.extend(Path(child.path) for child in children)
    return {'exists': True, 'allocated_bytes': total, 'entries': count,
            'elapsed_s': time.monotonic() - start}


def parse_memory(text):
    return {line.split(':',1)[0]: int(line.split()[1])*1024
            for line in text.splitlines() if len(line.split()) >= 3 and line.split()[2] == 'kB'}


def parse_vmstat(text):
    return {key:int(value) for key,value in (line.split() for line in text.splitlines())}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--graph-dir',action='append',required=True)
    parser.add_argument('--index-dir',action='append',required=True)
    parser.add_argument('--pid',type=int,action='append',required=True)
    parser.add_argument('--output',required=True)
    parser.add_argument('--before',help='Optional earlier JSON snapshot, including disk-before-1m.json')
    args=parser.parse_args()
    if len(args.graph_dir) != 3:
        parser.error('Specify exactly three graph directories for the required three storaged processes')
    target=Path(args.output)
    if target.exists():
        parser.error('Use a fresh output path')
    if any(not Path(p).is_dir() for p in args.graph_dir):
        parser.error('Every graph directory must exist')
    paths=[Path(p).absolute() for p in args.graph_dir+args.index_dir]
    for i,path in enumerate(paths):
        for other in paths[i+1:]:
            if path == other or path in other.parents or other in path.parents:
                parser.error('Directory roots must be distinct and non-overlapping to avoid double counting')
    report={'utc':stamp(), 'kind':'between_stage_system_snapshot', 'complete':False,
            'directory_details':{}, 'allocated_bytes':{}, 'processes':[],
            'page_size_bytes':os.sysconf('SC_PAGE_SIZE'),
            'note':'Directory st_blocks include files and directories, do not follow symlinks. Measurements are sequential, not atomic or peak. Sum three replicas only once.'}
    target.parent.mkdir(parents=True,exist_ok=True)
    try:
        report['meminfo']=Path('/proc/meminfo').read_text()
        report['vmstat']=Path('/proc/vmstat').read_text()
        report['memory_bytes']=parse_memory(report['meminfo'])
        report['memory_bytes']['SwapUsed']=report['memory_bytes']['SwapTotal']-report['memory_bytes']['SwapFree']
        vm=parse_vmstat(report['vmstat'])
        report['vmstat_selected']={key:vm.get(key) for key in ['pswpin','pswpout','oom_kill','pgmajfault','pgscan_direct','pgscan_kswapd']}
        for pid in dict.fromkeys(args.pid):
            try:
                row={'pid':pid, 'alive':True, **read_process(pid)}
            except (FileNotFoundError,ProcessLookupError):
                row={'pid':pid, 'alive':False}
            report['processes'].append(row)
        report['all_processes_alive']=all(row['alive'] for row in report['processes'])
        report['pss_complete']=report['all_processes_alive'] and all(row.get('pss_bytes') is not None for row in report['processes'])
        report['sum_service_pss_bytes']=sum(row['pss_bytes'] for row in report['processes']) if report['pss_complete'] else None
        report['sum_service_rss_bytes']=sum(row.get('rss_bytes',0) for row in report['processes'])
        for path in paths:
            info=allocated(path)
            report['directory_details'][str(path)]=info
            report['allocated_bytes'][str(path)]=info['allocated_bytes']
            parent=path
            while not parent.exists():
                parent=parent.parent
            report['directory_details'][str(path)]['filesystem_free_bytes']=shutil.disk_usage(parent).free
        report['graph_allocated_bytes']=sum(report['allocated_bytes'][str(Path(p).absolute())] for p in args.graph_dir)
        report['index_allocated_bytes']=sum(report['allocated_bytes'][str(Path(p).absolute())] for p in args.index_dir)
        if args.before:
            before=json.loads(Path(args.before).read_text())
            old=before['allocated_bytes']
            if set(old) != set(report['allocated_bytes']):
                raise ValueError('Before/after directory sets differ; refusing a misleading delta')
            deltas={path:value-old[path] for path,value in report['allocated_bytes'].items()}
            previous_vm=parse_vmstat(before['vmstat']) if isinstance(before.get('vmstat'),str) else before['vmstat_selected']
            report['delta']={'before':args.before,'allocated_bytes':deltas,
                'graph_allocated_bytes':sum(deltas[str(Path(p).absolute())] for p in args.graph_dir),
                'index_allocated_bytes':sum(deltas[str(Path(p).absolute())] for p in args.index_dir),
                'pswpin_pages':vm['pswpin']-previous_vm['pswpin'],
                'pswpout_pages':vm['pswpout']-previous_vm['pswpout'],
                'oom_kill':vm['oom_kill']-previous_vm['oom_kill']}
            report['delta']['pswpin_bytes']=report['delta']['pswpin_pages']*report['page_size_bytes']
            report['delta']['pswpout_bytes']=report['delta']['pswpout_pages']*report['page_size_bytes']
        report['complete']=report['all_processes_alive'] and report['pss_complete']
    except Exception as error:
        report['error']=str(error)
        raise
    finally:
        report['finished_utc']=stamp()
        target.write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps({k:report.get(k) for k in ['utc','finished_utc','complete','graph_allocated_bytes','index_allocated_bytes','sum_service_pss_bytes','delta','error']},indent=2))
    if not report['complete']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
