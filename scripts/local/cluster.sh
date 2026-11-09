#!/usr/bin/env bash
# Local cluster control (all processes on 127.0.0.1). Each run gets its own directory, so nothing is
# ever deleted:  build/local-run/<run-id>/{data,logs,pids,driver_events.jsonl}
#
#   scripts/local/cluster.sh start [config]     start metadata, admin, all storage nodes, routers
#   scripts/local/cluster.sh stop               SIGTERM every process of the current run
#   scripts/local/cluster.sh kill <node>        kill -9 one storage node (logs driver.kill)
#   scripts/local/cluster.sh restart <node>     start a killed node again (logs driver.start)
#   scripts/local/cluster.sh status             dkv_cli status
#   scripts/local/cluster.sh env                print DKV_RUN_DIR / CONFIG of the current run
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="${DKV_BIN:-$ROOT/build/src}"
RUNS="$ROOT/build/local-run"
CURRENT="$RUNS/current"     # symlink to the active run

now_ms() { date +%s%3N; }
cfg_nodes() { python3 -c "import json,sys;print(' '.join(n['id'] for n in json.load(open(sys.argv[1]))['storageNodes']))" "$1"; }
cfg_routers() { python3 -c "import json,sys;print(' '.join(n['id'] for n in json.load(open(sys.argv[1]))['routers']))" "$1"; }

load_current() {
  [[ -L "$CURRENT" ]] || { echo "no current run (start one first)" >&2; exit 1; }
  export DKV_RUN_DIR="$(readlink -f "$CURRENT")"
  CONFIG="$(cat "$DKV_RUN_DIR/config.path")"
}

start_proc() {  # name, command...
  local name="$1"; shift
  "$@" > "$DKV_RUN_DIR/logs/$name.log" 2>&1 &
  echo $! > "$DKV_RUN_DIR/pids/$name.pid"
}

driver_event() {  # event, node
  echo "{\"t_wall_ms\":$(now_ms),\"proc\":\"driver\",\"event\":\"$1\",\"node\":\"$2\"}" >> "$DKV_RUN_DIR/driver_events.jsonl"
}

case "${1:-}" in
  start)
    CONFIG="$(readlink -f "${2:-$ROOT/config/cluster.local.json}")"
    mkdir -p "$RUNS"
    export DKV_RUN_DIR="$RUNS/$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$DKV_RUN_DIR/logs" "$DKV_RUN_DIR/pids"
    echo "$CONFIG" > "$DKV_RUN_DIR/config.path"
    ln -sfn "$DKV_RUN_DIR" "$CURRENT"
    start_proc metadata "$BIN/dkv_metadata" --config "$CONFIG"
    start_proc admin "$BIN/dkv_admin" --config "$CONFIG" --out "$DKV_RUN_DIR/admin"
    sleep 0.3
    for n in $(cfg_nodes "$CONFIG"); do start_proc "node-$n" "$BIN/dkv_node" --config "$CONFIG" --id "$n"; done
    for r in $(cfg_routers "$CONFIG"); do start_proc "router-$r" "$BIN/dkv_router" --config "$CONFIG" --id "$r"; done
    sleep 1.5
    echo "run dir: $DKV_RUN_DIR"
    ;;
  stop)
    load_current
    for f in "$DKV_RUN_DIR"/pids/*.pid; do
      pid="$(cat "$f")"; kill -TERM "$pid" 2>/dev/null || true
    done
    sleep 1
    for f in "$DKV_RUN_DIR"/pids/*.pid; do
      pid="$(cat "$f")"; kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null || true
    done
    echo "stopped run $DKV_RUN_DIR"
    ;;
  kill)
    load_current
    n="${2:?node id}"; pid="$(cat "$DKV_RUN_DIR/pids/node-$n.pid")"
    driver_event driver.kill "$n"
    kill -KILL "$pid"
    echo "killed node $n (pid $pid)"
    ;;
  restart)
    load_current
    n="${2:?node id}"
    mv "$DKV_RUN_DIR/logs/node-$n.log" "$DKV_RUN_DIR/logs/node-$n.before-$(now_ms).log" 2>/dev/null || true
    driver_event driver.start "$n"
    start_proc "node-$n" "$BIN/dkv_node" --config "$CONFIG" --id "$n"
    echo "restarted node $n (pid $(cat "$DKV_RUN_DIR/pids/node-$n.pid"))"
    ;;
  status)
    load_current
    "$BIN/dkv_cli" --config "$CONFIG" status
    ;;
  env)
    load_current
    echo "DKV_RUN_DIR=$DKV_RUN_DIR"
    echo "CONFIG=$CONFIG"
    ;;
  *)
    sed -n '2,12p' "$0"; exit 2 ;;
esac
