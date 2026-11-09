#!/usr/bin/env bash
# Local kill/restart experiment (HLD §11, proposal §5 "Main experiment").
#   KEYS=2000 RATE=300 BASE_S=10 DOWN_S=15 POST_S=10 VICTIM=B scripts/local/experiment.sh [config]
# Steps: start cluster -> preload keys -> start load -> kill VICTIM -> keep load running -> restart
# VICTIM -> wait for REPAIR_DONE -> keep load running -> stop load -> verify -> analyze.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="${DKV_BIN:-$ROOT/build/src}"
CONFIG="$(readlink -f "${1:-$ROOT/config/cluster.local.json}")"
KEYS="${KEYS:-2000}"; RATE="${RATE:-300}"; THREADS="${THREADS:-16}"
BASE_S="${BASE_S:-10}"; DOWN_S="${DOWN_S:-15}"; POST_S="${POST_S:-10}"
VICTIM="${VICTIM:-B}"; VALUE_BYTES="${VALUE_BYTES:-256}"; PUT_RATIO="${PUT_RATIO:-0.5}"
REPAIR_TIMEOUT_S="${REPAIR_TIMEOUT_S:-120}"

CLUSTER="$ROOT/scripts/local/cluster.sh"
"$CLUSTER" start "$CONFIG"
eval "$("$CLUSTER" env)"
RUN="$DKV_RUN_DIR"
cat > "$RUN/experiment.json" <<EOF
{"keys":$KEYS,"rate":$RATE,"threads":$THREADS,"base_s":$BASE_S,"down_s":$DOWN_S,"post_s":$POST_S,
 "victim":"$VICTIM","value_bytes":$VALUE_BYTES,"put_ratio":$PUT_RATIO,"config":"$CONFIG"}
EOF
trap '"$CLUSTER" stop >/dev/null 2>&1 || true' EXIT

echo "== preload $KEYS keys"
"$BIN/dkv_loadgen" --config "$CONFIG" --out "$RUN/preload" --keys "$KEYS" --threads "$THREADS" \
  --rate 0 --preload --value-bytes "$VALUE_BYTES"

echo "== load: $RATE ops/s, put ratio $PUT_RATIO"
"$BIN/dkv_loadgen" --config "$CONFIG" --out "$RUN/load" --keys "$KEYS" --threads "$THREADS" \
  --rate "$RATE" --duration 100000 --put-ratio "$PUT_RATIO" --value-bytes "$VALUE_BYTES" \
  --seq-base 1000000 --run-until-file "$RUN/stop_load" > "$RUN/logs/loadgen.log" 2>&1 &
LOAD_PID=$!

sleep "$BASE_S"
echo "== kill $VICTIM"
"$CLUSTER" kill "$VICTIM"
sleep "$DOWN_S"
echo "== restart $VICTIM"
"$CLUSTER" restart "$VICTIM"
echo "== waiting for REPAIR_DONE"
deadline=$(( $(date +%s) + REPAIR_TIMEOUT_S ))
until grep -q "^[0-9]*,$VICTIM,2," "$RUN/admin/repairs.csv" 2>/dev/null; do
  if (( $(date +%s) > deadline )); then echo "repair did not finish in ${REPAIR_TIMEOUT_S}s" >&2; break; fi
  sleep 0.2
done
grep "^[0-9]*,$VICTIM,2," "$RUN/admin/repairs.csv" || true
sleep "$POST_S"
touch "$RUN/stop_load"
wait "$LOAD_PID" || true
cat "$RUN/logs/loadgen.log"

echo "== verify acknowledged writes"
"$BIN/dkv_verify" --config "$CONFIG" --dir "$RUN/preload,$RUN/load" || true

echo "== analyze"
python3 "$ROOT/scripts/analysis/analyze_run.py" "$RUN" || true
echo "run dir: $RUN"
