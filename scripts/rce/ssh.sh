#!/bin/sh
# Run a command on the RCE login node using the credentials in secrets/rce.json.
# Usage: scripts/rce/ssh.sh '<remote command>' [< local-file]
#   stdin is forwarded, so a job script can be submitted without copying it first:
#   scripts/rce/ssh.sh 'sbatch' < scripts/rce/m0_toolchain.sbatch
# For an interactive shell use plain `ssh <username>` instead.
# Works without sshpass: OpenSSH >= 8.4 reads the password from SSH_ASKPASS when SSH_ASKPASS_REQUIRE=force.
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
TARGET=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['username'])" "$ROOT/secrets/rce.json")
SSH_ASKPASS="$HERE/askpass.sh" SSH_ASKPASS_REQUIRE=force DISPLAY="${DISPLAY:-:0}" \
  exec ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=15 \
      -o PreferredAuthentications=password,keyboard-interactive -o NumberOfPasswordPrompts=1 \
      -o LogLevel=ERROR "$TARGET" "$@"
