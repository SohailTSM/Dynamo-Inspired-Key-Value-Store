#!/bin/bash
# Kill/restart experiment on RCE compute nodes (HLD §10-§11). Nothing runs on the login node.
#   scripts/rce/ssh.sh 'sbatch --export=ALL,KEYS_LIST="1000 10000",RATES_LIST="200 1000"' < scripts/rce/experiment.sbatch
# Env knobs (defaults in brackets):
#   KEYS_LIST ["1000 10000"]  RATES_LIST ["200 1000"]  REPEATS [1]  BASE_S [20]  DOWN_S [30]  POST_S [20]
#   VICTIM [auto = a storage node not on the control host]  PUT_RATIO [0.5]  VALUE_BYTES [256]
#   TWO_DOWN [0]  -> also run one "beyond tolerance" scenario with two storage nodes down
# Results: ~/dkv-results/<jobid>/run-*/  (+ summary.md). Node data lives in /tmp on each node and is
# removed at the end of the job (only this job's own directory).
#SBATCH -J dkv-exp
#SBATCH -N 4
#SBATCH --ntasks-per-node=4
#SBATCH -c 2
#SBATCH -t 01:30:00
#SBATCH -o dkv-exp-%j.out
set -uo pipefail

BIN="$HOME/dynamo-kv/build/src"
SRC="$HOME/dynamo-kv/src"
RESULTS="$HOME/dkv-results/$SLURM_JOB_ID"
DATA_BASE="/tmp/$USER/dkv-$SLURM_JOB_ID"
KEYS_LIST="${KEYS_LIST:-1000 10000}"; RATES_LIST="${RATES_LIST:-200 1000}"; REPEATS="${REPEATS:-1}"
BASE_S="${BASE_S:-20}"; DOWN_S="${DOWN_S:-30}"; POST_S="${POST_S:-20}"
PUT_RATIO="${PUT_RATIO:-0.5}"; VALUE_BYTES="${VALUE_BYTES:-256}"; THREADS="${THREADS:-16}"
REPAIR_TIMEOUT_S="${REPAIR_TIMEOUT_S:-180}"; TWO_DOWN="${TWO_DOWN:-0}"
mkdir -p "$RESULTS"

HOSTS="$(scontrol show hostnames "$SLURM_JOB_NODELIST" | tr '\n' ' ')"
echo "== job $SLURM_JOB_ID on: $HOSTS (driver on $(hostname))"
python3 "$SRC/scripts/rce/make_config.py" --hosts "$HOSTS" --out "$RESULTS/cluster.json" \
  --placement "$RESULTS/placement.json" --data-dir "$DATA_BASE/\${DKV_RUN_TAG}"
CONFIG="$RESULTS/cluster.json"
pl() { python3 -c "import json,sys;d=json.load(open(sys.argv[2]));print(eval(sys.argv[1]))" "$1" "$RESULTS/placement.json"; }
CONTROL="$(pl "d['control']")"; ROUTER_HOST="$(pl "d['router']['R1']")"; LOADGEN_HOST="$(pl "d['loadgen']")"
STORAGE_IDS="$(pl "' '.join(sorted(d['storage']))")"
host_of() { pl "d['storage']['$1']"; }
if [[ "${VICTIM:-auto}" == auto ]]; then
  VICTIM=""; for id in $STORAGE_IDS; do [[ "$(host_of "$id")" != "$CONTROL" ]] && VICTIM="$id"; done
fi
echo "control=$CONTROL router=$ROUTER_HOST loadgen=$LOADGEN_HOST storage=[$STORAGE_IDS] victim=$VICTIM"

now_ms() { date +%s%3N; }
driver_event() { echo "{\"t_wall_ms\":$(now_ms),\"proc\":\"driver\",\"event\":\"$1\",\"node\":\"$2\"}" >> "$RUN/driver_events.jsonl"; }

# start_proc <name> <host> <cpus> <cmd...>: background srun step; the remote shell writes its PID, then
# execs. CPUs matter: srun pins a step to its cores, and a storage node with -c1 shares one core between
# its gRPC server and fan-out pool (first RCE run: p50 10 ms with -c1).
start_proc() {
  local name="$1" host="$2" cpus="$3"; shift 3
  srun --overlap -N1 -n1 -c"$cpus" -w "$host" --output="$RUN/logs/$name.log" \
    bash -c 'echo $$ > "$0"; exec "$@"' "$RUN/pids/$name.pid" "$@" &
  echo $! > "$RUN/pids/$name.srun"
}
signal_proc() {  # <name> <host> <signal>
  local pid; pid="$(cat "$RUN/pids/$1.pid" 2>/dev/null)" || return 0
  srun --overlap -N1 -n1 -c1 -w "$2" kill "-$3" "$pid" 2>/dev/null || true
}
wait_pidfile() { for _ in $(seq 1 600); do [[ -s "$RUN/pids/$1.pid" ]] && return 0; sleep 0.1; done; return 1; }

stop_cluster() {
  # Restarted victims use their own pidfile name (node-X.r): reusing a name made NFS serve the stale PID.
  for id in $STORAGE_IDS; do
    signal_proc "node-$id" "$(host_of "$id")" TERM
    signal_proc "node-$id.r" "$(host_of "$id")" TERM
  done
  signal_proc router-R1 "$ROUTER_HOST" TERM
  signal_proc admin "$CONTROL" TERM
  signal_proc metadata "$CONTROL" TERM
  sleep 2
  for f in "$RUN"/pids/*.srun; do kill "$(cat "$f")" 2>/dev/null || true; done
  wait 2>/dev/null || true
}

collect_node_logs() {
  mkdir -p "$RUN/nodes"
  for id in $STORAGE_IDS; do
    srun --overlap -N1 -n1 -c1 -w "$(host_of "$id")" \
      cp "$DATA_BASE/$DKV_RUN_TAG/$id/events.jsonl" "$RUN/nodes/$id-events.jsonl" 2>/dev/null || true
  done
}

# run_one <tag> <keys> <rate> <victims...>
run_one() {
  local tag="$1" keys="$2" rate="$3"; shift 3
  local victims=("$@")
  export DKV_RUN_TAG="$tag"
  RUN="$RESULTS/$tag"
  mkdir -p "$RUN/logs" "$RUN/pids"
  echo "{\"keys\":$keys,\"rate\":$rate,\"threads\":$THREADS,\"base_s\":$BASE_S,\"down_s\":$DOWN_S,\"post_s\":$POST_S,\"victim\":\"${victims[0]}\",\"victims\":\"${victims[*]}\",\"value_bytes\":$VALUE_BYTES,\"put_ratio\":$PUT_RATIO,\"hosts\":\"$HOSTS\"}" > "$RUN/experiment.json"
  echo "== $tag: keys=$keys rate=$rate victims=${victims[*]}"

  start_proc metadata "$CONTROL" 1 "$BIN/dkv_metadata" --config "$CONFIG"
  start_proc admin "$CONTROL" 1 "$BIN/dkv_admin" --config "$CONFIG" --out "$RUN/admin"
  sleep 1
  for id in $STORAGE_IDS; do start_proc "node-$id" "$(host_of "$id")" 4 "$BIN/dkv_node" --config "$CONFIG" --id "$id"; done
  start_proc router-R1 "$ROUTER_HOST" 2 "$BIN/dkv_router" --config "$CONFIG" --id R1
  sleep 4

  srun --overlap -N1 -n1 -c4 -w "$LOADGEN_HOST" "$BIN/dkv_loadgen" --config "$CONFIG" --out "$RUN/preload" \
    --keys "$keys" --threads "$THREADS" --rate 0 --preload --value-bytes "$VALUE_BYTES" | tee "$RUN/logs/preload.log"
  start_proc loadgen "$LOADGEN_HOST" 4 "$BIN/dkv_loadgen" --config "$CONFIG" --out "$RUN/load" --keys "$keys" \
    --threads "$THREADS" --rate "$rate" --duration 100000 --put-ratio "$PUT_RATIO" --value-bytes "$VALUE_BYTES" \
    --seq-base 1000000
  wait_pidfile loadgen || echo "WARN: loadgen pidfile missing"

  sleep "$BASE_S"
  for v in "${victims[@]}"; do
    wait_pidfile "node-$v"
    driver_event driver.kill "$v"
    signal_proc "node-$v" "$(host_of "$v")" KILL
    echo "killed $v at $(now_ms)"
  done
  sleep "$DOWN_S"
  for v in "${victims[@]}"; do
    driver_event driver.start "$v"
    start_proc "node-$v.r" "$(host_of "$v")" 4 "$BIN/dkv_node" --config "$CONFIG" --id "$v"
    echo "restarted $v at $(now_ms)"
  done
  local deadline=$(( $(date +%s) + REPAIR_TIMEOUT_S ))
  for v in "${victims[@]}"; do
    until grep -q "^[0-9]*,$v,2," "$RUN/admin/repairs.csv" 2>/dev/null; do
      (( $(date +%s) > deadline )) && { echo "WARN: repair of $v not reported within ${REPAIR_TIMEOUT_S}s"; break; }
      sleep 0.2
    done
    grep "^[0-9]*,$v,2," "$RUN/admin/repairs.csv" || true
  done
  sleep "$POST_S"
  signal_proc loadgen "$LOADGEN_HOST" TERM
  wait "$(cat "$RUN/pids/loadgen.srun")" 2>/dev/null || true
  cat "$RUN/logs/loadgen.log"

  srun --overlap -N1 -n1 -c4 -w "$LOADGEN_HOST" "$BIN/dkv_verify" --config "$CONFIG" \
    --dir "$RUN/preload,$RUN/load" | tee "$RUN/logs/verify.log"
  stop_cluster
  collect_node_logs
  python3 "$SRC/scripts/analysis/analyze_run.py" "$RUN" | tee "$RUN/analysis.txt"
}

for r in $(seq 1 "$REPEATS"); do
  for keys in $KEYS_LIST; do
    for rate in $RATES_LIST; do
      run_one "run-k${keys}-r${rate}-${r}" "$keys" "$rate" "$VICTIM"
    done
  done
done
if [[ "$TWO_DOWN" == 1 ]]; then
  second=""; for id in $STORAGE_IDS; do [[ "$id" != "$VICTIM" && "$(host_of "$id")" != "$CONTROL" ]] && second="$id"; done
  [[ -n "$second" ]] && run_one "run-twodown-k$(echo $KEYS_LIST | awk '{print $1}')" "$(echo $KEYS_LIST | awk '{print $1}')" \
    "$(echo $RATES_LIST | awk '{print $1}')" "$VICTIM" "$second"
fi

python3 "$SRC/scripts/analysis/summarize.py" "$RESULTS" | tee "$RESULTS/summary.md"
# Remove this job's node-local data (only our own directory).
for h in $HOSTS; do srun --overlap -N1 -n1 -c1 -w "$h" rm -rf -- "$DATA_BASE" 2>/dev/null || true; done
echo "== done: $RESULTS"
