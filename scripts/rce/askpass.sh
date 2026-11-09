#!/bin/sh
# SSH_ASKPASS helper: prints the RCE password from secrets/rce.json (never stored in this script).
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
exec python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['password'])" "$ROOT/secrets/rce.json"
