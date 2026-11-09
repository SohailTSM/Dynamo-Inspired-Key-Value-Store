#!/usr/bin/env bash
# Local multi-process integration tests (LLD §11.2). Each test uses a fresh cluster run directory.
#   scripts/local/integration_tests.sh            # all tests
#   scripts/local/integration_tests.sh durability # tests whose name contains the filter
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="$ROOT/build/src"
CFG="$ROOT/config/cluster.local.json"
CL="$ROOT/scripts/local/cluster.sh"
FILTER="${1:-}"
PASS=0; FAIL=0; RESULTS=()

cli() { "$BIN/dkv_cli" --config "$CFG" "$@"; }
put_range() { local p=$1 a=$2 b=$3 bad=0; for i in $(seq "$a" "$b"); do cli put "$p$i" "v$i" >/dev/null || bad=$((bad+1)); done; echo "$bad"; }
check_range() {  # prefix from to -> number of keys NOT returning v<i>
  local p=$1 a=$2 b=$3 bad=0 out
  for i in $(seq "$a" "$b"); do out=$(cli get "$p$i"); [[ "$out" == "OK value=v$i "* ]] || bad=$((bad+1)); done
  echo "$bad"
}
loadgen() { "$BIN/dkv_loadgen" --config "$CFG" "$@" 2>&1 | tail -1; }
verify() { "$BIN/dkv_verify" --config "$CFG" --dir "$1" 2>&1 | tail -1; }
run_dir() { readlink -f "$ROOT/build/local-run/current"; }
expect() {  # name, condition-result (0 = pass), detail
  if [[ "$2" == 0 ]]; then PASS=$((PASS+1)); RESULTS+=("PASS  $1  $3"); else FAIL=$((FAIL+1)); RESULTS+=("FAIL  $1  $3"); fi
}
want() { [[ -z "$FILTER" || "$1" == *"$FILTER"* ]]; }

# 1. Smoke: PUT/GET 200 keys.
if want smoke; then
  "$CL" start >/dev/null
  bad_put=$(put_range s 1 200); bad_get=$(check_range s 1 200)
  expect smoke $(( bad_put + bad_get )) "put failures=$bad_put get mismatches=$bad_get"
  "$CL" stop >/dev/null
fi

# 2. Durability: write, kill -9 EVERY storage node, restart all, read back.
if want durability; then
  "$CL" start >/dev/null
  bad_put=$(put_range d 1 300)
  for n in A B C D; do "$CL" kill $n >/dev/null; done
  sleep 1
  for n in A B C D; do "$CL" restart $n >/dev/null; done
  sleep 3
  bad_get=$(check_range d 1 300)
  expect durability $(( bad_put + bad_get )) "all 4 nodes kill -9 + restart: put failures=$bad_put lost=$bad_get"
  "$CL" stop >/dev/null
fi

# 3. Beyond tolerance: two nodes down -> some keys unavailable (explicit error), none lost after restart.
if want two_down; then
  "$CL" start >/dev/null
  bad_put=$(put_range t 1 200)
  "$CL" kill B >/dev/null; "$CL" kill C >/dev/null
  sleep 4
  ok=0; err=0
  for i in $(seq 1 200); do out=$(cli get "t$i"); [[ "$out" == OK* ]] && ok=$((ok+1)) || err=$((err+1)); done
  "$CL" restart B >/dev/null; "$CL" restart C >/dev/null
  sleep 4
  lost=$(check_range t 1 200)
  # Expect: some keys failed while 2 were down (those with B and C in their preference list), all back after.
  cond=$(( (err > 0 && ok > 0 && lost == 0) ? 0 : 1 ))
  expect two_down $cond "2 of 4 down: $ok readable, $err QUORUM_NOT_REACHED; after restart lost=$lost"
  "$CL" stop >/dev/null
fi

# 4. Control-plane outages: stop metadata and admin; data path keeps working.
if want control_plane; then
  "$CL" start >/dev/null
  R=$(run_dir)
  kill -TERM "$(cat "$R/pids/metadata.pid")" "$(cat "$R/pids/admin.pid")"
  sleep 1
  bad_put=$(put_range c 1 100); bad_get=$(check_range c 1 100)
  expect control_plane $(( bad_put + bad_get )) "metadata+admin stopped: put failures=$bad_put get mismatches=$bad_get"
  "$CL" stop >/dev/null
fi

# 5. Router failover / coordinator kill under load: kill a node mid-load, verify no acked write lost.
if want load_kill; then
  "$CL" start >/dev/null
  R=$(run_dir)
  ( "$BIN/dkv_loadgen" --config "$CFG" --out "$R/load" --keys 500 --threads 8 --rate 400 --duration 12 \
      --put-ratio 0.7 > "$R/logs/loadgen.log" 2>&1 ) &
  lp=$!
  sleep 3; "$CL" kill A >/dev/null; sleep 5; "$CL" restart A >/dev/null
  wait $lp
  v=$(verify "$R/load")
  lost=$(python3 -c "import json;print(json.load(open('$R/load/verify_summary.json'))['lost'])")
  expect load_kill "$lost" "$(tail -1 "$R/logs/loadgen.log") | $v"
  "$CL" stop >/dev/null
fi

# 6. Fault injector: A cannot reach B; writes coordinated by A still reach W via the other replica.
if want faults; then
  "$CL" start >/dev/null
  cli faults A drop-out B >/dev/null
  bad_put=$(put_range f 1 150); bad_get=$(check_range f 1 150)
  cli faults A clear >/dev/null
  expect faults $(( bad_put + bad_get )) "A->B dropped: put failures=$bad_put get mismatches=$bad_get"
  "$CL" stop >/dev/null
fi

# 7. Torn writes: kill -9 a node repeatedly during heavy writes; it must always recover.
if want torn; then
  "$CL" start >/dev/null
  R=$(run_dir)
  ( "$BIN/dkv_loadgen" --config "$CFG" --out "$R/load" --keys 300 --threads 8 --rate 800 --duration 14 \
      --put-ratio 1.0 > "$R/logs/loadgen.log" 2>&1 ) &
  lp=$!
  fails=0
  for i in 1 2 3 4 5; do
    sleep 1; "$CL" kill C >/dev/null; sleep 0.5; "$CL" restart C >/dev/null; sleep 1
    pid=$(cat "$R/pids/node-C.pid"); kill -0 "$pid" 2>/dev/null || fails=$((fails+1))
  done
  wait $lp
  sleep 2
  v=$(verify "$R/load")
  lost=$(python3 -c "import json;print(json.load(open('$R/load/verify_summary.json'))['lost'])")
  truncs=$(grep -h "recovery.done" "$R"/data/C/events.jsonl | grep -c '"truncated_bytes":[1-9]' || true)
  expect torn $(( fails + lost )) "5 kill/restart cycles: restart failures=$fails, recoveries with torn tail=$truncs | $v"
  "$CL" stop >/dev/null
fi

echo
printf '%s\n' "${RESULTS[@]}"
echo "== $PASS passed, $FAIL failed"
[[ $FAIL == 0 ]]
