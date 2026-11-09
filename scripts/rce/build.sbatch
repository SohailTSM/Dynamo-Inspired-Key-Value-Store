#!/bin/bash
# Builds the project on a compute node into ~/dynamo-kv/build (NFS-shared /home) and runs unit tests.
#   scripts/rce/sync.sh && scripts/rce/ssh.sh 'sbatch --wait' < scripts/rce/build.sbatch
#SBATCH -J dkv-build
#SBATCH -N 1
#SBATCH -c 16
#SBATCH -t 00:30:00
#SBATCH -o dkv-build-%j.out
set -euo pipefail
module load cmake/3.20.3 gRPC/1.74.1
SRC="$HOME/dynamo-kv/src"
BUILD="$HOME/dynamo-kv/build"
echo "== build on $(hostname): $(g++ --version | head -1), $(cmake --version | head -1)"
t0=$(date +%s)
cmake -S "$SRC" -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH="$GRPC_HOME" > "$BUILD.cmake.log" 2>&1 \
  || { tail -50 "$BUILD.cmake.log"; exit 1; }
if ! cmake --build "$BUILD" -j 16 > "$BUILD.build.log" 2>&1; then
  echo "parallel build failed; retrying serially to get a clean error message"
  cmake --build "$BUILD" -j 1 > "$BUILD.build1.log" 2>&1 || { tail -60 "$BUILD.build1.log"; exit 1; }
  echo "(serial retry succeeded: the parallel failure was transient)"
fi
echo "build OK in $(( $(date +%s) - t0 ))s"
echo "compiler warnings: $(grep -c "warning:" "$BUILD.build.log" || true)"
ls -la "$BUILD"/src/dkv_* | awk '{print $5, $9}'
echo "== unit tests"
TESTTMP="/tmp/$USER/dkv-tests-$SLURM_JOB_ID"
mkdir -p "$TESTTMP"
DKV_TEST_TMPDIR="$TESTTMP" "$BUILD/tests/dkv_tests" 2>&1 | tail -40
rmdir "$TESTTMP" 2>/dev/null || true
