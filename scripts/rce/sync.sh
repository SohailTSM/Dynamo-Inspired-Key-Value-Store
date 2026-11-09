#!/bin/sh
# Copies the source tree to RCE:~/dynamo-kv/src (file transfer only; nothing is built on the login node).
# Usage: scripts/rce/sync.sh
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
TARGET=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['username'])" "$ROOT/secrets/rce.json")
export SSH_ASKPASS="$HERE/askpass.sh" SSH_ASKPASS_REQUIRE=force DISPLAY="${DISPLAY:-:0}"
SSH="ssh -o StrictHostKeyChecking=accept-new -o PreferredAuthentications=password,keyboard-interactive -o NumberOfPasswordPrompts=1 -o LogLevel=ERROR"
rsync -az -e "$SSH" --rsync-path="mkdir -p dynamo-kv/src && rsync" \
  --exclude /build/ --exclude /tools/ --exclude /secrets/ --exclude /.git/ --exclude '/docs/*/diagrams/out/' \
  --exclude '*.pdf' --exclude Transcript.txt --exclude test-tmp/ \
  "$ROOT/" "$TARGET:dynamo-kv/src/"
echo "synced to $TARGET:~/dynamo-kv/src"
