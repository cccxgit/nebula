#!/usr/bin/env python3
"""Collect native TEXT_SEARCH/ES_QUERY hit sets, then compare them offline.

Never starts services or changes ES settings. A max-hits+1 sentinel LIMIT avoids
ES's implicit default of ten hits; hitting the sentinel is an error, not success.
ES requires max_result_window >= sentinel; this tool never raises it silently.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import sys
import time
import urllib.request

QUERIES = [('term_water', 'water'), ('phrase_new_york', '"New York"'),
           ('boolean_water_river', 'water AND river')]


def stamp():
    return time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')


def identifier(value):
    if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', value):
        raise ValueError('Invalid identifier')
    return '`' + value + '`'


def statement(function, index, text, sentinel):
    return (f'LOOKUP ON passage WHERE {function}({identifier(index)}, {json.dumps(text)}) '
            f'YIELD id(vertex) AS vid, score() AS score | LIMIT {sentinel};')


def decode(value):
    return value.decode() if isinstance(value, bytes) else value


def plan_evidence(plan):
    nodes = []
    for node in getattr(plan, 'plan_node_descs', None) or []:
        name = decode(node.name)
        if 'FulltextIndexScan' in name:
            nodes.append({'name': name, **{decode(item.key): decode(item.value)
                          for item in getattr(node, 'description', None) or []}})
    return nodes


def es_count(index, text):
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    request = urllib.request.Request('http://127.0.0.1:19200/' + index + '/_count',
        data=json.dumps({'query': {'query_string': {'query': text}}}).encode(),
        headers={'Content-Type': 'application/json'}, method='POST')
    with opener.open(request, timeout=60) as response:
        result = json.load(response)
    shards = result.get('_shards', {})
    if ('error' in result or result.get('timed_out') or shards.get('failed') != 0 or
            shards.get('successful') != shards.get('total') or not isinstance(result.get('count'), int)):
        raise ValueError('Incomplete ES count response: ' + json.dumps(result))
    return result


def store_ids(path, ids):
    content = ''.join(json.dumps(vid, ensure_ascii=False) + '\n' for vid in sorted(ids))
    path.write_text(content)
    return hashlib.sha256(content.encode()).hexdigest()


def collect(args):
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError('Use a fresh output directory')
    manifest = json.loads(Path(args.manifest).read_text())
    fingerprint = {key: manifest[key] for key in ('rows', 'normalized_jsonl_sha256',
                   'max_chars_setting', 'max_text_bytes_setting')}
    report = {'utc': stamp(), 'kind': 'native_complete_hit_sets', 'backend': args.backend,
              'space': args.space, 'index': args.index, 'host': args.host,
              'port': args.port or (9669 if args.backend == 'tantivy' else 29669),
              'dataset': fingerprint, 'max_hits': args.max_hits, 'sentinel_limit': args.max_hits + 1,
              'all_queries_complete': False, 'queries': [],
              'note': 'Correctness only; no concurrent writes allowed. Scores are checked for finiteness, not cross-engine equality.'}
    write_json(output / 'source-manifest.json', manifest)
    pool, client = None, None
    try:
        from nebula3.Config import Config
        from nebula3.gclient.net import ConnectionPool
        config = Config()
        config.timeout = args.timeout_ms
        config.max_connection_pool_size = 2
        pool = ConnectionPool()
        if not pool.init([(args.host, report['port'])], config):
            raise RuntimeError('Could not connect to graphd')
        client = pool.get_session('root', os.environ.get('NEBULA_PASSWORD', 'nebula'))
        used = client.execute('USE ' + identifier(args.space))
        if not used.is_succeeded():
            raise RuntimeError(str(used.error_msg()))
        for name, text in QUERIES:
            query = statement('TEXT_SEARCH' if args.backend == 'tantivy' else 'ES_QUERY',
                              args.index, text, args.max_hits + 1)
            entry = {'name': name, 'query_text': text, 'ngql': query, 'complete': False}
            started = time.perf_counter()
            try:
                # EXPLAIN cannot fetch documents. Refuse an ES plan that would fall back to size=10.
                explained = client.execute('EXPLAIN ' + query)
                if not explained.is_succeeded():
                    raise RuntimeError('EXPLAIN: ' + str(explained.error_msg()))
                plans = plan_evidence(explained.plan_desc())
                entry['fulltext_plan'] = plans
                (output / (name + '.explain.txt')).write_text(str(explained.plan_desc()))
                if len(plans) != 1:
                    raise ValueError('Expected exactly one FulltextIndexScan')
                if args.backend == 'es' and plans[0].get('limit') != str(args.max_hits + 1):
                    raise ValueError('ES FulltextIndexScan limit not pushed; complete collection would be unsafe')
                if args.backend == 'tantivy' and plans[0].get('engine') != 'tantivy':
                    raise ValueError('Tantivy engine not proven in plan')
                if args.backend == 'es':
                    entry['es_count_before'] = es_count(args.index, text)
                    if entry['es_count_before']['count'] > args.max_hits:
                        raise ValueError('ES total hits exceed max-hits safety bound')
                result = client.execute('PROFILE ' + query)
                entry['error_code'] = result.error_code()
                if not result.is_succeeded():
                    raise RuntimeError('Query failed or returned partial success: ' + str(result.error_msg()))
                (output / (name + '.profile.txt')).write_text(str(result.plan_desc()))
                rows = result.column_values('vid')
                scores = result.column_values('score')
                if any(not value.is_string() for value in rows):
                    raise ValueError('Expected string VIDs')
                ids = [value.as_string() for value in rows]
                if len(scores) != len(ids) or any(not math.isfinite(float(value.cast())) for value in scores):
                    raise ValueError('Missing or non-finite score')
                entry['returned_rows'] = len(ids)
                entry['unique_ids'] = len(set(ids))
                entry['ids_file'] = name + '.ids.jsonl'
                entry['ids_sha256'] = store_ids(output / entry['ids_file'], ids)
                if len(ids) != len(set(ids)):
                    raise ValueError('Duplicate VID in engine response')
                if len(ids) >= args.max_hits + 1:
                    raise ValueError('Sentinel result limit reached; complete hit set not established')
                if args.backend == 'es':
                    entry['es_count_after'] = es_count(args.index, text)
                    if not (entry['es_count_before']['count'] == len(ids) == entry['es_count_after']['count']):
                        raise ValueError('ES count and returned VID count differ; possible truncation, timeout, or concurrent writes')
                entry['complete'] = True
            except Exception as error:
                entry['error'] = str(error)
            entry['elapsed_s'] = time.perf_counter() - started
            report['queries'].append(entry)
            write_json(output / 'collection.json', report)
            print(json.dumps({'name': name, 'complete': entry['complete'],
                              'count': entry.get('returned_rows'), 'error': entry.get('error')}, ensure_ascii=False), flush=True)
        report['all_queries_complete'] = all(row['complete'] for row in report['queries'])
    except Exception as error:
        report['error'] = str(error)
    finally:
        if client is not None:
            client.release()
        if pool is not None:
            pool.close()
        report['finished_utc'] = stamp()
        write_json(output / 'collection.json', report)
    if not report['all_queries_complete']:
        raise SystemExit(1)


def load_set(root, query):
    path = root / query['ids_file']
    content = path.read_bytes()
    if hashlib.sha256(content).hexdigest() != query['ids_sha256']:
        raise ValueError('Stored VID file hash mismatch')
    ids = [json.loads(line) for line in content.decode().splitlines()]
    if ids != sorted(set(ids)) or len(ids) != query['returned_rows']:
        raise ValueError('Stored VID file is not the complete sorted unique set')
    return set(ids)


def compare(args):
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError('Use a fresh output directory')
    left, right = Path(args.left), Path(args.right)
    a, b = json.loads((left/'collection.json').read_text()), json.loads((right/'collection.json').read_text())
    report = {'utc': stamp(), 'kind': 'offline_native_hit_set_comparison', 'left': str(left),
              'right': str(right), 'all_equal': False, 'queries': []}
    try:
        if {a['backend'], b['backend']} != {'tantivy', 'es'}:
            raise ValueError('Expected one Tantivy and one ES collection')
        if a['dataset'] != b['dataset']:
            raise ValueError('Input manifests/projections differ')
        if not a['all_queries_complete'] or not b['all_queries_complete']:
            raise ValueError('At least one engine collection failed or was incomplete')
        for name, text in QUERIES:
            x = next(q for q in a['queries'] if q['name'] == name)
            y = next(q for q in b['queries'] if q['name'] == name)
            if not x['complete'] or not y['complete'] or x['query_text'] != text or y['query_text'] != text:
                raise ValueError('Incomplete or changed query: ' + name)
            xs, ys = load_set(left, x), load_set(right, y)
            only_left, only_right = xs-ys, ys-xs
            store_ids(output/(name+'.only-left.ids.jsonl'), only_left)
            store_ids(output/(name+'.only-right.ids.jsonl'), only_right)
            report['queries'].append({'name': name, 'query_text': text, 'left_count': len(xs),
                                      'right_count': len(ys), 'only_left_count':len(only_left),
                                      'only_right_count':len(only_right), 'equal':xs==ys})
        report['all_equal'] = all(q['equal'] for q in report['queries'])
    except Exception as error:
        report['error'] = str(error)
    write_json(output/'comparison.json', report)
    print(json.dumps(report, indent=2))
    if not report['all_equal']:
        raise SystemExit(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    c = sub.add_parser('collect')
    c.add_argument('--backend', choices=['tantivy','es'], required=True)
    c.add_argument('--manifest', required=True)
    c.add_argument('--space', required=True)
    c.add_argument('--index', required=True)
    c.add_argument('--host', choices=['127.0.0.1'], default='127.0.0.1')
    c.add_argument('--port', type=int)
    c.add_argument('--timeout-ms', type=int, default=60000)
    c.add_argument('--max-hits', type=int, default=100000)
    c.add_argument('--output', required=True)
    c = sub.add_parser('compare')
    c.add_argument('--left', required=True)
    c.add_argument('--right', required=True)
    c.add_argument('--output', required=True)
    args = parser.parse_args()
    if args.command == 'collect' and not 1 <= args.max_hits <= 100000:
        parser.error('max-hits must be 1..100000 for the local resource budget')
    globals()[args.command](args)


if __name__ == '__main__':
    main()
