#!/usr/bin/env python3
"""Prepare and operate an isolated ES7 + original Nebula baseline.

prepare never starts a process. start checks its assigned ports and launches only
this dedicated runtime. stop verifies PID command lines belong to this runtime.
"""
import argparse
import json
import os
from pathlib import Path
import resource
import signal
import socket
import subprocess
import time
import urllib.request

DEFAULT_NEBULA = "/home/sch/nebula/nebula-3.6-io/build"
DEFAULT_ES = "/home/sch/nebula-data/tantivy/es-baseline/elasticsearch-7.17.29"
DEFAULT_RUNTIME = "/home/sch/nebula-run/3.6-tantivy-es-baseline"
NODES = [("metad", "nebula-metad", 29559, 39559),
         ("storage1", "nebula-storaged", 29779, 39779),
         ("storage2", "nebula-storaged", 29789, 39789),
         ("storage3", "nebula-storaged", 29799, 39799),
         ("listener", "nebula-storaged", 29879, 39879),
         ("graphd", "nebula-graphd", 29669, 39669)]


def event(runtime_dir, event_name, **data):
    row = {"utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "event": event_name, **data}
    text = json.dumps(row)
    print(text, flush=True)
    path = runtime_dir / "operation-log.jsonl"
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a") as output:
        output.write(text + "\n")


def baseline_environment():
    """Only child services bypass proxies; never modify the caller/system environment."""
    env = {key: value for key, value in os.environ.items()
           if not key.lower().endswith("_proxy")}
    env["no_proxy"] = env["NO_PROXY"] = "127.0.0.1,localhost"
    return env


def prepare(args):
    root = Path(args.runtime)
    resources = root / "share" / "resources"
    resources.mkdir(parents=True, exist_ok=True)
    for filename in ("date_time_zonespec.csv", "gflags.json"):
        source = Path(args.nebula_home).parent / "resources" / filename
        (resources / filename).write_bytes(source.read_bytes())
    for name in ("etc", "pids", "logs", "data", "es-config", "es-data", "es-logs"):
        (root / name).mkdir(parents=True, exist_ok=True)
    for name, binary, port, web in NODES:
        (root / "logs" / name).mkdir(exist_ok=True)
        (root / "data" / name).mkdir(exist_ok=True)
        flags = {"daemonize": "true", "pid_file": root / "pids" / (name + ".pid"),
                 "log_dir": root / "logs" / name, "minloglevel": 0, "v": 0,
                 "redirect_stdout": "true", "local_ip": "127.0.0.1",
                 "meta_server_addrs": "127.0.0.1:29559", "port": port,
                 "ws_ip": "127.0.0.1", "ws_http_port": web,
                 "heartbeat_interval_secs": 1}
        if name != "graphd":
            flags["data_path"] = root / "data" / name
        else:
            flags.update(enable_authorize="false", local_config="true", enable_optimizer="true",
                         num_worker_threads=4, num_netio_threads=2,
                         system_memory_high_watermark_ratio=0.8,
                         accept_partial_success="false", enable_udf="false",
                         max_allowed_query_size=4194304)
        if name.startswith("storage"):
            flags.update(local_config="true", rocksdb_block_cache=64,
                         rocksdb_compression="lz4", wal_ttl=3600,
                         num_io_threads=2, num_worker_threads=4,
                         raft_heartbeat_interval_secs=5)
        if name == "listener":
            flags.update(local_config="true", listener_path=root / "data" / "listener-wal",
                         engine_type="rocksdb", rocksdb_block_cache=4,
                         num_io_threads=2, num_worker_threads=4,
                         raft_heartbeat_interval_secs=5,
                         # BatchHolder::size() counts key/value bytes, not rows.
                         listener_commit_batch_size=1048576,
                         listener_commit_interval_secs=1)
        if name == "metad":
            flags.update(default_parts_num=20, default_replica_factor=3)
        (root / "etc" / (name + ".conf")).write_text(
            "\n".join(f"--{key}={value}" for key, value in flags.items()) + "\n")
    es = {"cluster.name": "nebula-tantivy-es-baseline", "node.name": "es-baseline-local",
          "network.host": "127.0.0.1", "http.port": 19200, "transport.port": 19300,
          "discovery.type": "single-node", "xpack.security.enabled": "false",
          "xpack.ml.enabled": "false", "ingest.geoip.downloader.enabled": "false",
          "path.data": root / "es-data", "path.logs": root / "es-logs"}
    (root / "es-config" / "elasticsearch.yml").write_text(
        "\n".join(f"{key}: {value}" for key, value in es.items()) + "\n")
    es_home = Path(args.es_home)
    for filename in ("jvm.options", "log4j2.properties"):
        source = es_home / "config" / filename
        if source.exists():
            (root / "es-config" / filename).write_bytes(source.read_bytes())
    (root / "es-config" / "jvm.options.d").mkdir(exist_ok=True)
    (root / "es-config" / "jvm.options.d" / "heap.options").write_text(
        f"-Xms{args.es_heap_mb}m\n-Xmx{args.es_heap_mb}m\n")
    source = Path(args.nebula_home).parent
    try:
        git_revision = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
        git_changes = subprocess.check_output(["git", "-C", str(source), "status", "--short"], text=True)
    except subprocess.CalledProcessError:
        git_revision, git_changes = "unavailable", "unavailable"
    binary_versions = {}
    for binary in ("nebula-graphd", "nebula-metad", "nebula-storaged"):
        path = Path(args.nebula_home) / "bin" / binary
        binary_versions[binary] = subprocess.check_output([str(path), "--version"],
                                                          text=True, stderr=subprocess.STDOUT).strip()
    settings = {"runtime": str(root), "nebula_home": args.nebula_home, "es_home": args.es_home,
                "baseline_source_revision": git_revision, "baseline_source_status": git_changes,
                "baseline_binary_versions": binary_versions,
                "es_heap_mb": args.es_heap_mb, "graph_port": 29669, "meta_port": 29559,
                "storage_ports": [29779, 29789, 29799], "listener_port": 29879,
                "es_http_port": 19200, "partition_num": 20, "replica_factor": 3,
                "listener_commit_batch_bytes": 1048576, "listener_commit_interval_secs": 1,
                "build_comparison_note": "Default baseline is existing 3.6-io Debug build. Embedded binary versions are recorded separately from current source HEAD, which may differ. Record local modifications and identical compiler/runtime settings before claiming controlled speedup."}
    (root / "settings.json").write_text(json.dumps(settings, indent=2) + "\n")
    event(root, "prepared", **settings)


def ensure_free(ports):
    for port in ports:
        with socket.socket() as sock:
            try:
                sock.bind(("127.0.0.1", port))
            except OSError as error:
                raise RuntimeError(f"Port {port} is occupied; no services started") from error


def wait_port(port, seconds=60):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1):
                return
        except OSError:
            time.sleep(.5)
    raise RuntimeError(f"Port {port} did not become ready")


def process_state(root, name):
    """A PID file alone is not proof of a live process owned by this runtime."""
    path = root / "pids" / (name + ".pid")
    pid = int(path.read_text().strip()) if path.exists() else None
    result = {"name": name, "pid": pid, "running": False}
    if pid is None:
        return result
    try:
        proc = Path("/proc") / str(pid)
        argv = (proc / "cmdline").read_bytes().decode(errors="replace").split("\0")
        state = (proc / "stat").read_text().rsplit(")", 1)[1].split()[0]
    except (FileNotFoundError, ProcessLookupError):
        return result
    if name == "elasticsearch":
        owned = ("-Des.path.conf=" + str(root / "es-config")) in argv
        owned = owned and "org.elasticsearch.bootstrap.Elasticsearch" in argv
    else:
        binary = next(node[1] for node in NODES if node[0] == name)
        owned = ("--flagfile=" + str(root / "etc" / (name + ".conf"))) in argv
        owned = owned and bool(argv) and Path(argv[0]).name == binary
    result.update(running=owned and state not in ("Z", "X"), owned=owned,
                  process_state=state)
    return result


def elasticsearch_state(root):
    result = process_state(root, "elasticsearch")
    try:
        identity = es_request("", timeout=3)
        nodes = es_request("_nodes/_local/process", timeout=3)
        actual_pids = [node["process"]["id"] for node in nodes.get("nodes", {}).values()]
        result.update(http_ready=identity.get("cluster_name") == "nebula-tantivy-es-baseline",
                      http_cluster_name=identity.get("cluster_name"),
                      http_version=identity.get("version", {}).get("number"),
                      http_process_ids=actual_pids,
                      pid_matches_http=actual_pids == [result["pid"]])
        result["ready"] = (result["running"] and result["http_ready"] and
                           result["pid_matches_http"])
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        result.update(http_ready=False, ready=False, http_error=str(error))
    return result


def start_es(args):
    """Recover only the dedicated ES service, without touching the six Nebula services."""
    root = Path(args.runtime)
    settings = json.loads((root / "settings.json").read_text())
    if process_state(root, "elasticsearch")["running"]:
        raise RuntimeError("Dedicated ES process is already running; inspect status before restarting")
    ensure_free([19200, 19300])
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    if hard != resource.RLIM_INFINITY and hard < 65536:
        raise RuntimeError("File descriptor hard limit is below required 65536")
    resource.setrlimit(resource.RLIMIT_NOFILE, (max(soft, 65536), hard))
    env = baseline_environment()
    env["ES_PATH_CONF"] = str(root / "es-config")
    env.pop("JAVA_HOME", None)
    event(root, "es_start_requested", detached_session=True, local_proxy_bypass=True)
    with (root / "logs" / "es-launch.log").open("a") as output:
        subprocess.run([str(Path(settings["es_home"]) / "bin" / "elasticsearch"),
                        "-d", "-p", str(root / "pids" / "elasticsearch.pid")],
                       cwd=root, env=env, stdin=subprocess.DEVNULL, stdout=output,
                       stderr=subprocess.STDOUT, start_new_session=True, check=True)
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        state = elasticsearch_state(root)
        if state["ready"]:
            event(root, "es_ready", **state)
            return
        time.sleep(.5)
    raise RuntimeError("ES did not become HTTP/PID ready: " + json.dumps(state))


def start(args):
    root = Path(args.runtime)
    settings = json.loads((root / "settings.json").read_text())
    ports = [19200, 19300]
    for name, binary, port, web in NODES:
        ports.extend([port, web])
        if name != "graphd":
            ports.extend([port - 2, port - 1, port + 1])
    ensure_free(ports)
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    if hard != resource.RLIM_INFINITY and hard < 65536:
        raise RuntimeError("File descriptor hard limit is below required 65536")
    resource.setrlimit(resource.RLIMIT_NOFILE, (max(soft, 65536), hard))
    event(root, "start_requested", settings=settings)
    start_es(args)
    env = baseline_environment()
    # A new storaged will not finish startup until ADD HOSTS registers it.
    # Bring graphd up directly after metad, register all three hosts, then start storage.
    ordered = [NODES[0], NODES[-1], *NODES[1:-1]]
    for name, binary, port, web in ordered:
        with (root / "logs" / (name + "-launch.log")).open("a") as output:
            subprocess.run([str(Path(settings["nebula_home"]) / "bin" / binary),
                            "--flagfile=" + str(root / "etc" / (name + ".conf"))],
                           cwd=root, env=env, stdout=output, stderr=subprocess.STDOUT, check=True)
        wait_port(port)
        event(root, "node_started", name=name, port=port, local_proxy_bypass=True)
        if name == "graphd":
            from nebula3.Config import Config
            from nebula3.gclient.net import ConnectionPool
            config = Config()
            config.timeout = 60000
            pool = ConnectionPool()
            if not pool.init([("127.0.0.1", 29669)], config):
                raise RuntimeError("Baseline graphd became unavailable before host registration")
            client = pool.get_session("root", os.environ.get("NEBULA_PASSWORD", "nebula"))
            try:
                result = client.execute('ADD HOSTS 127.0.0.1:29779,127.0.0.1:29789,127.0.0.1:29799;')
                if not result.is_succeeded() and "exist" not in str(result.error_msg()).lower():
                    raise RuntimeError(str(result.error_msg()))
                event(root, "storage_hosts_registered")
            finally:
                client.release()
                pool.close()


def stop(args):
    root = Path(args.runtime)
    pending = {}
    # Only stop processes whose command line contains this dedicated runtime.
    for name in [node[0] for node in reversed(NODES)] + ["elasticsearch"]:
        pid_file = root / "pids" / (name + ".pid")
        if not pid_file.exists():
            continue
        pid = int(pid_file.read_text().strip())
        proc = Path("/proc") / str(pid)
        if not proc.exists():
            continue
        cmdline = (proc / "cmdline").read_bytes().replace(b"\0", b" ").decode(errors="replace")
        if str(root) not in cmdline:
            raise RuntimeError(f"PID {pid} does not belong to this baseline; refusing to stop it")
        os.kill(pid, signal.SIGTERM)
        pending[pid] = name
        event(root, "stop_sent", name=name, pid=pid)
    deadline = time.monotonic() + 90
    while pending and time.monotonic() < deadline:
        for pid, name in list(pending.items()):
            try:
                command = (Path('/proc') / str(pid) / 'cmdline').read_bytes()
            except (FileNotFoundError, ProcessLookupError):
                command = b''
            if str(root).encode() not in command:
                event(root, "node_stopped", name=name, pid=pid)
                del pending[pid]
        if pending:
            time.sleep(.25)
    if pending:
        raise RuntimeError(f"Services have not exited; refusing to claim stopped: {pending}")


def status(args):
    root = Path(args.runtime)
    for name in [node[0] for node in NODES] + ["elasticsearch"]:
        result = elasticsearch_state(root) if name == "elasticsearch" else process_state(root, name)
        print(json.dumps(result))


def initialize(args):
    from nebula3.Config import Config
    from nebula3.gclient.net import ConnectionPool
    root = Path(args.runtime)
    config = Config()
    config.timeout = 60000
    pool = ConnectionPool()
    if not pool.init([("127.0.0.1", 29669)], config):
        raise RuntimeError("Baseline graphd is unavailable")
    client = pool.get_session("root", os.environ.get("NEBULA_PASSWORD", "nebula"))
    def execute(query, retry_seconds=0, allowed_error=""):
        deadline = time.monotonic() + retry_seconds
        while True:
            result = client.execute(query)
            if result.is_succeeded():
                event(root, "ngql_ok", query=query)
                return result
            error = str(result.error_msg())
            if allowed_error and allowed_error.lower() in error.lower():
                event(root, "ngql_already_configured", query=query, error=error)
                return result
            if time.monotonic() >= deadline:
                raise RuntimeError(query + ": " + error)
            time.sleep(1)
    try:
        execute('ADD HOSTS 127.0.0.1:29779,127.0.0.1:29789,127.0.0.1:29799;',
                allowed_error="exist")
        execute('SIGN IN TEXT SERVICE (127.0.0.1:19200, HTTP);', allowed_error="exist")
        # Preserve the native ES default of one primary shard; a single ES node
        # cannot place a replica, so explicitly use zero rather than yellow shards.
        request = urllib.request.Request('http://127.0.0.1:19200/_template/nebula_ft_benchmark',
            data=json.dumps({"index_patterns": ["nebula_index_ft_*"],
                             "settings": {"number_of_shards": 1, "number_of_replicas": 0}}).encode(),
            headers={"Content-Type": "application/json"}, method="PUT")
        with urllib.request.urlopen(request, timeout=30) as response:
            event(root, "es_template", response=json.load(response))
        execute('CREATE SPACE IF NOT EXISTS ft_bench (partition_num=20, replica_factor=3, vid_type=FIXED_STRING(64));',
                retry_seconds=60)
        execute('USE ft_bench;', retry_seconds=60)
        execute('CREATE TAG IF NOT EXISTS passage(text string);')
        execute('ADD LISTENER ELASTICSEARCH 127.0.0.1:29879;', retry_seconds=30,
                allowed_error="exist")
        if args.analyzer not in ("keyword", "standard"):
            raise ValueError("Baseline analyzer must be keyword or standard")
        execute('CREATE FULLTEXT TAG INDEX nebula_index_ft_passage ON passage(text) ANALYZER="' +
                args.analyzer + '";', retry_seconds=60, allowed_error="exist")
        event(root, "initialized", graph_partition_num=20, graph_replica_factor=3,
              es_primary_shards=1, es_replicas=0, analyzer=args.analyzer)
    finally:
        client.release()
        pool.close()


class StageSession:
    """Small phased baseline workflow; the existing initialize command is unchanged."""
    def __init__(self, args):
        from nebula3.Config import Config
        from nebula3.gclient.net import ConnectionPool
        self.args, self.root = args, Path(args.runtime)
        config = Config()
        config.timeout = 60000
        self.pool = ConnectionPool()
        if not self.pool.init([('127.0.0.1',29669)],config):
            raise RuntimeError('Baseline graphd is unavailable')
        self.client=self.pool.get_session('root',os.environ.get('NEBULA_PASSWORD','nebula'))

    def execute(self, query):
        start=time.perf_counter()
        result=self.client.execute(query)
        event(self.root,'phased_ngql',query=query,success=result.is_succeeded(),
              elapsed_s=time.perf_counter()-start,
              error='' if result.is_succeeded() else str(result.error_msg()))
        if not result.is_succeeded():
            raise RuntimeError(str(result.error_msg()))
        return result

    def wait(self, callback, label):
        deadline,last=time.monotonic()+self.args.wait_seconds,None
        while time.monotonic()<deadline:
            try:
                return callback()
            except (AssertionError,RuntimeError) as error:
                last=str(error)
                event(self.root,'phased_wait',label=label,error=last)
                time.sleep(2)
        raise RuntimeError(f'Timed out waiting for {label}: {last}')

    def topology(self):
        result=self.execute('SHOW PARTS')
        assert result.row_size()==20,'Expected 20 partitions'
        for i in range(result.row_size()):
            part,leader,peers,lost=[v.cast() for v in result.row_values(i)]
            assert leader and len(peers.split(','))==3 and not lost,f'Unhealthy partition {part}'
        event(self.root,'phased_topology_healthy',partition_num=20,replica_factor=3)

    def close(self):
        self.client.release()
        self.pool.close()


def safe_identifier(value):
    import re
    if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*',value):
        raise ValueError('Invalid identifier')
    return '`'+value+'`'


def es_request(path, body=None, method='GET', timeout=60):
    opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
    request=urllib.request.Request('http://127.0.0.1:19200/'+path,
        data=json.dumps(body).encode() if body is not None else None,method=method,
        headers={'Content-Type':'application/json'})
    with opener.open(request,timeout=timeout) as response:
        result=json.load(response)
    if 'error' in result:
        raise RuntimeError(json.dumps(result))
    return result


def metadata_guard_delay(flag_response):
    """Bounded time allowance, never a claim that a remote cache is ready."""
    values = {row['name']: row.get('value') for row in flag_response.get('flags', [])}
    heartbeat = values.get('heartbeat_interval_secs')
    if heartbeat is None or int(heartbeat) <= 0:
        raise ValueError('Listener heartbeat interval is unavailable or invalid')
    intervals = [int(heartbeat)]
    if values.get('load_meta_interval_secs') is not None:
        if int(values['load_meta_interval_secs']) <= 0:
            raise ValueError('Listener metadata refresh interval is invalid')
        intervals.append(int(values['load_meta_interval_secs']))
    seconds = max(5, 2 * max(intervals) + 2)
    if seconds > 30:
        raise ValueError('Two metadata cycles plus margin exceed the 30-second preparation bound')
    return seconds


def listener_metadata_guard(root):
    url = ('http://127.0.0.1:39879/flags?'
           'flags=load_meta_interval_secs,heartbeat_interval_secs&format=json')
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(url, timeout=5) as response:
        flags = json.load(response)
    seconds = metadata_guard_delay(flags)
    event(root, 'phased_metadata_time_guard_start', listener_url=url, flags=flags,
          planned_wait_seconds=seconds, max_wait_seconds=30,
          note='Time allowance only; does not prove Listener metadata readiness. Complete count and VID checks remain mandatory.')
    start = time.perf_counter()
    time.sleep(seconds)
    elapsed = time.perf_counter() - start
    event(root, 'phased_metadata_time_guard_end', elapsed_s=elapsed,
          planned_wait_seconds=seconds)
    return elapsed


def prepare_space(args):
    """Create a new graph space/tag/tag-index, without FT metadata or Listener assignment."""
    session=StageSession(args)
    try:
        def hosts_ready():
            result=session.execute('SHOW HOSTS')
            rows=[result.row_values(i) for i in range(result.row_size())]
            ready={(row[0].as_string(),row[1].as_int()) for row in rows
                   if row[0].is_string() and row[1].is_int() and row[2].as_string()=='ONLINE'}
            assert {('127.0.0.1',p) for p in [29779,29789,29799]}<=ready,'Storage hosts are not online'
        session.wait(hosts_ready,'three online storage hosts')
        session.execute(f'CREATE SPACE {safe_identifier(args.space)} '
                        '(partition_num=20,replica_factor=3,vid_type=FIXED_STRING(64))')
        session.wait(lambda:session.execute('USE '+safe_identifier(args.space)),'space metadata')
        session.wait(session.topology,'healthy topology')
        session.execute('CREATE TAG passage(text string)')
        session.wait(lambda:session.execute('CREATE TAG INDEX passage_tag_lookup ON passage()'),
                     'graph local tag schema cache')
        session.wait(lambda:session.execute('DESCRIBE TAG INDEX passage_tag_lookup'),'tag index metadata')
        session.wait(lambda:session.execute('FETCH PROP ON passage "__readiness_probe__" YIELD id(vertex) AS vid'),
                     'storage tag schema')
        event(session.root,'phased_space_ready',space=args.space,fulltext_created=False,
              listener_assigned=False,note='Import graph data now; create-index performs later backfill.')
    finally:
        session.close()


def create_index(args):
    """Attach Listener and create/backfill an ES index after the graph-only import."""
    if args.expected_rows is None or args.expected_rows<1:
        raise ValueError('create-index requires --expected-rows from the exact input manifest')
    if not args.index.startswith('nebula_index_ft_'):
        raise ValueError('ES baseline index must use the nebula_index_ft_ prefix')
    safe_identifier(args.index)
    session=StageSession(args)
    started=time.perf_counter()
    try:
        session.execute('USE '+safe_identifier(args.space))
        session.wait(session.topology,'healthy topology')
        try:
            session.execute('SIGN IN TEXT SERVICE (127.0.0.1:19200, HTTP)')
        except RuntimeError as error:
            if 'exist' not in str(error).lower():
                raise
        template={'index_patterns':['nebula_index_ft_*'],
                  'settings':{'number_of_shards':1,'number_of_replicas':0,
                              'max_result_window':args.result_window}}
        event(session.root,'phased_es_template',response=es_request('_template/nebula_ft_benchmark',template,'PUT'),
              settings=template['settings'])
        def listener_ready():
            result=session.execute('SHOW LISTENER')
            assert result.row_size()==20,'Expected listener for all 20 partitions'
            assert {v.as_int() for v in result.column_values('PartId')}==set(range(1,21)), 'Unexpected listener partition set'
            assert all(v.as_string()=='ELASTICSEARCH' for v in result.column_values('Type')), 'Unexpected listener type'
            # HostAddr::toString quotes the hostname; permit its raw rendering too.
            assert all(v.as_string() in ('"127.0.0.1":29879','127.0.0.1:29879')
                       for v in result.column_values('Host')), 'Unexpected listener host'
            assert all(v.as_string()=='ONLINE' for v in result.column_values('Status')),'Listener offline'
            return result
        try:
            session.execute('ADD LISTENER ELASTICSEARCH 127.0.0.1:29879')
        except RuntimeError as error:
            # The old MetaClient maps E_EXISTED to exactly "Existed!". Do not
            # reinterpret transport, parsing, or other ADD failures as success.
            if not str(error).strip().endswith('Existed!'):
                raise
            session.wait(listener_ready,'existing listener assignment validation')
            event(session.root,'phased_listener_reused_for_retry',space=args.space,
                  host='127.0.0.1:29879',partitions=20,original_error=str(error))
        session.wait(listener_ready,'listener metadata/heartbeat')
        session.wait(lambda:session.execute(f'CREATE FULLTEXT TAG INDEX {safe_identifier(args.index)} '
                     f'ON passage(text) ANALYZER="{args.analyzer}"'),'fulltext DDL/schema cache')
        settings=es_request(args.index+'/_settings')
        event(session.root,'phased_es_index_settings',index=args.index,settings=settings)
        actual=settings[args.index]['settings']['index']
        if (int(actual.get('number_of_shards',0)) != 1 or
                int(actual.get('number_of_replicas',-1)) != 0 or
                int(actual.get('max_result_window',10000)) < args.result_window):
            raise ValueError('ES index shard/replica/result-window settings do not match the requested baseline')
        metadata_guard_seconds = listener_metadata_guard(session.root)
        rebuild_started = time.perf_counter()
        job=session.execute('REBUILD FULLTEXT INDEX')
        job_id=job.row_values(0)[0].as_int()
        def job_done():
            result=session.execute(f'SHOW JOB {job_id}')
            states=[v.as_string() for v in result.column_values('Status')]
            states=[v for v in states if not v.startswith('Failed:')]
            if any(v in ('FAILED','STOPPED') for v in states):
                raise ValueError(f'ES rebuild job {job_id} failed: {states}')
            assert len(states)>1 and all(v=='FINISHED' for v in states),states
        session.wait(job_done,'ES listener rebuild job completion')
        def complete_count():
            response=es_request(args.index+'/_count')
            shards=response.get('_shards',{})
            assert shards.get('failed')==0 and shards.get('successful')==shards.get('total'),'Partial ES count'
            assert response.get('count')==args.expected_rows,response
            return response
        count=session.wait(complete_count,'ES refresh-visible complete document count')
        event(session.root,'phased_index_visible',space=args.space,index=args.index,count=count,
              elapsed_s=time.perf_counter()-started,
              metadata_time_guard_elapsed_s=metadata_guard_seconds,
              rebuild_to_visible_elapsed_s=time.perf_counter()-rebuild_started,
              note='Total includes Listener assignment, DDL, explicit metadata time allowance, rebuild and normal refresh. Rebuild-to-visible excludes the explicit allowance but includes job polling/refresh; neither is pure ES engine build time. Run native_compare next.')
    finally:
        session.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["prepare", "start", "start-es", "stop", "status", "initialize", "prepare-space", "create-index"])
    parser.add_argument("--runtime", default=DEFAULT_RUNTIME)
    parser.add_argument("--nebula-home", default=DEFAULT_NEBULA)
    parser.add_argument("--es-home", default=DEFAULT_ES)
    parser.add_argument("--es-heap-mb", type=int, default=512)
    parser.add_argument("--analyzer", default="standard", choices=["keyword", "standard"])
    parser.add_argument("--space", default="ft_bench")
    parser.add_argument("--index", default="nebula_index_ft_passage")
    parser.add_argument("--expected-rows", type=int)
    parser.add_argument("--result-window", type=int, default=100001)
    parser.add_argument("--wait-seconds", type=int, default=3600)
    args = parser.parse_args()
    if not 10001 <= args.result_window <= 100001:
        parser.error("result-window must be 10001..100001 for the local memory budget")
    globals()[args.command.replace("-", "_")](args)


if __name__ == "__main__":
    main()
