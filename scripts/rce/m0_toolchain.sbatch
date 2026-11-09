#!/bin/bash
# Milestone 0 — toolchain check on RCE compute nodes (nothing is built or run on the login node).
# Submit from this repo:  scripts/rce/ssh.sh 'sbatch --wait' < scripts/rce/m0_toolchain.sbatch
# Checks: gRPC module + CMake + C++17 build, cross-node gRPC call and latency,
#         shared /home, local /tmp filesystem, fsync latency, clock sync.
#SBATCH -J kv-m0
#SBATCH -N 2
#SBATCH --ntasks-per-node=1
#SBATCH -c 8
#SBATCH -t 00:20:00
#SBATCH -o kv-m0-%j.out
set -euo pipefail

echo "== job $SLURM_JOB_ID on $SLURM_JOB_NODELIST (batch host $(hostname))"
# Compute nodes have no system cmake (the login node does), so load the module.
module load cmake/3.20.3 gRPC/1.74.1
command -v cmake >/dev/null || { echo "cmake still missing after module load"; exit 1; }
echo "GRPC_HOME=$GRPC_HOME  g++: $(g++ --version | head -1)  cmake: $(cmake --version | head -1)"

W="$HOME/dynamo-kv-m0"
rm -rf "$W"
mkdir -p "$W/src"
cd "$W/src"

cat > hello.proto <<'EOF'
syntax = "proto3";
package m0;
service Echo { rpc Ping(PingReq) returns (PingResp); }
message PingReq  { string from = 1; }
message PingResp { string host = 1; }
EOF

cat > server.cc <<'EOF'
#include <grpcpp/grpcpp.h>
#include <unistd.h>
#include <iostream>
#include "hello.grpc.pb.h"

class EchoImpl final : public m0::Echo::Service {
  grpc::Status Ping(grpc::ServerContext*, const m0::PingReq*, m0::PingResp* resp) override {
    char h[256] = {0};
    gethostname(h, sizeof(h) - 1);
    resp->set_host(h);
    return grpc::Status::OK;
  }
};

int main(int argc, char** argv) {
  std::string addr = argc > 1 ? argv[1] : "0.0.0.0:50551";
  EchoImpl svc;
  grpc::ServerBuilder b;
  b.AddListeningPort(addr, grpc::InsecureServerCredentials());
  b.RegisterService(&svc);
  auto server = b.BuildAndStart();
  if (!server) { std::cerr << "server failed to start on " << addr << std::endl; return 1; }
  std::cout << "server listening on " << addr << std::endl;
  server->Wait();
}
EOF

cat > client.cc <<'EOF'
#include <grpcpp/grpcpp.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>
#include "hello.grpc.pb.h"

int main(int argc, char** argv) {
  if (argc < 2) { std::cerr << "usage: client host:port" << std::endl; return 2; }
  auto ch = grpc::CreateChannel(argv[1], grpc::InsecureChannelCredentials());
  if (!ch->WaitForConnected(std::chrono::system_clock::now() + std::chrono::seconds(15))) {
    std::cerr << "could not connect to " << argv[1] << std::endl; return 1;
  }
  auto stub = m0::Echo::NewStub(ch);
  std::vector<double> us;
  std::string host;
  for (int i = 0; i < 2000; ++i) {
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    m0::PingReq rq; rq.set_from("client");
    m0::PingResp rs;
    auto t0 = std::chrono::steady_clock::now();
    grpc::Status st = stub->Ping(&ctx, rq, &rs);
    auto t1 = std::chrono::steady_clock::now();
    if (!st.ok()) { std::cerr << "RPC failed: " << st.error_message() << std::endl; return 1; }
    host = rs.host();
    us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
  }
  std::sort(us.begin(), us.end());
  std::cout << "OK: 2000 RPCs answered by " << host
            << "  p50=" << us[us.size() / 2] << "us  p99=" << us[us.size() * 99 / 100] << "us" << std::endl;
}
EOF

cat > CMakeLists.txt <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(kv_m0 CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

set(GEN ${CMAKE_CURRENT_BINARY_DIR})
add_custom_command(
  OUTPUT ${GEN}/hello.pb.cc ${GEN}/hello.pb.h ${GEN}/hello.grpc.pb.cc ${GEN}/hello.grpc.pb.h
  COMMAND $<TARGET_FILE:protobuf::protoc>
          --cpp_out=${GEN} --grpc_out=${GEN}
          --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
          -I ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/hello.proto
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/hello.proto)

add_library(hello_proto ${GEN}/hello.pb.cc ${GEN}/hello.grpc.pb.cc)
target_include_directories(hello_proto PUBLIC ${GEN})
target_link_libraries(hello_proto PUBLIC gRPC::grpc++ protobuf::libprotobuf Threads::Threads)

add_executable(server server.cc)
target_link_libraries(server PRIVATE hello_proto)
add_executable(client client.cc)
target_link_libraries(client PRIVATE hello_proto)
EOF

echo "== build (on $(hostname))"
t0=$(date +%s)
cmake -S . -B ../build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$GRPC_HOME" > ../cmake.log 2>&1 \
  || { tail -40 ../cmake.log; exit 1; }
cmake --build ../build -j 8 > ../build.log 2>&1 || { tail -60 ../build.log; exit 1; }
echo "build OK in $(( $(date +%s) - t0 ))s; binary sizes: $(du -h ../build/server ../build/client | tr '\n' ' ')"
echo "dynamic deps of server:"; ldd ../build/server | sed 's/^/   /'

mapfile -t NODES < <(scontrol show hostnames "$SLURM_JOB_NODELIST")

echo "== per-node environment"
for n in "${NODES[@]}"; do
  srun -N1 -n1 -w "$n" bash -c '
    echo "-- $(hostname)  cpus=$(nproc)"
    echo "   home:  $(df -hT "$HOME" | tail -1 | awk "{print \$2, \$1, \$5\" used\"}")"
    echo "   /tmp:  $(df -hT /tmp | tail -1 | awk "{print \$2, \$1, \$4\" free\"}")"
    echo "   clock: $(timedatectl 2>/dev/null | grep -i "synchronized" | xargs)"
    python3 - <<PY
import os, time, statistics, tempfile
d = tempfile.mkdtemp(dir="/tmp"); p = os.path.join(d, "f")
fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
lat = []
for _ in range(200):
    t = time.perf_counter(); os.write(fd, b"x" * 4096); os.fsync(fd); lat.append((time.perf_counter() - t) * 1000)
os.close(fd); os.remove(p); os.rmdir(d)
lat.sort()
print("   fsync 4KiB on /tmp: p50=%.3fms p99=%.3fms" % (statistics.median(lat), lat[int(len(lat) * 0.99)]))
PY
  '
done

echo "== cross-node gRPC: server on ${NODES[0]}, client on ${NODES[1]}"
srun -N1 -n1 -w "${NODES[0]}" ../build/server 0.0.0.0:50551 &
SERVER=$!
sleep 3
srun -N1 -n1 -w "${NODES[1]}" ../build/client "${NODES[0]}:50551"
kill "$SERVER" 2>/dev/null || true
wait 2>/dev/null || true
echo "== M0 DONE"
