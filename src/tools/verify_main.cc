// dkv_verify --config <cluster.json> --dir <dir1>[,<dir2>...] [--threads 16]
//   Several loadgen output dirs are read in the given order (e.g. preload, then main run).
//
// "No acknowledged write is lost" check (proposal goal 8, LLD §7.8). For every key in acks_*.csv:
//   last  = last acknowledged PUT (seq)
//   later = PUTs of that key issued after `last` that returned an error/timeout ("maybe" writes)
//   PASS if GET returns exactly WorkloadValue(key, last.seq) or the value of a `later` PUT,
//   LOST otherwise (including NOT_FOUND / older value). Output: verify.csv + verify_summary.json.
#include <atomic>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include "client/kv_client.h"
#include "common/config_loader.h"
#include "common/fs.h"
#include "common/flags.h"
#include "rpc/rpc_util.h"
#include "tools/workload.h"

using namespace dkv;

struct KeyState {
  uint64_t last_acked = 0;          // 0 = never acked
  std::set<uint64_t> maybe_after;   // maybe seqs issued after last_acked
};

int main(int argc, char** argv) {
  Flags f(argc, argv);
  if (!f.has("config") || !f.has("dir")) {
    std::fprintf(stderr, "usage: dkv_verify --config <cluster.json> --dir <loadgen out dir>\n");
    return 2;
  }
  auto cfg = LoadClusterConfig(f.get("config"));
  if (!cfg.ok()) {
    std::fprintf(stderr, "%s\n", cfg.status().ToString().c_str());
    return 1;
  }
  const std::string dir = f.get("dir");
  const int threads = int(f.get_int("threads", 16));
  // Value size must match the loadgen run: read it from summary.json unless overridden.
  uint32_t value_bytes = 256;
  const std::string out_dir = dir.substr(dir.find_last_of(',') == std::string::npos ? 0 : dir.find_last_of(',') + 1);
  if (auto sum = ReadFile(JoinPath(out_dir, "summary.json")); sum.ok()) {
    auto pos = sum->find("\"value_bytes\":");
    if (pos != std::string::npos) value_bytes = uint32_t(std::strtoul(sum->c_str() + pos + 14, nullptr, 10));
  }
  value_bytes = uint32_t(f.get_int("value-bytes", value_bytes));

  // Parse ack logs. Lines of one worker are in issue order; a key belongs to exactly one worker.
  std::map<std::string, KeyState> keys;
  std::vector<std::string> dirs;
  {
    std::stringstream ds(dir);
    std::string d;
    while (std::getline(ds, d, ','))
      if (!d.empty()) dirs.push_back(d);
  }
  std::vector<std::string> ack_files;
  for (const auto& d : dirs) {
    auto names = ListDir(d);
    if (!names.ok()) {
      std::fprintf(stderr, "%s\n", names.status().ToString().c_str());
      return 1;
    }
    for (const auto& n : *names)
      if (n.rfind("acks_", 0) == 0) ack_files.push_back(JoinPath(d, n));
  }
  for (const auto& path : ack_files) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
      std::stringstream ss(line);
      std::string worker, key, seq_s, status;
      std::getline(ss, worker, ',');
      std::getline(ss, key, ',');
      std::getline(ss, seq_s, ',');
      std::getline(ss, status, ',');
      if (key.empty()) continue;
      uint64_t seq = std::stoull(seq_s);
      auto& ks = keys[key];
      if (status == "acked") {
        ks.last_acked = seq;
        ks.maybe_after.clear();
      } else {
        ks.maybe_after.insert(seq);
      }
    }
  }

  std::vector<std::pair<std::string, KeyState>> work;
  for (auto& kv : keys)
    if (kv.second.last_acked > 0) work.emplace_back(kv.first, kv.second);

  std::vector<std::string> routers;
  for (const auto& r : cfg->routers()) routers.push_back(EndpointAddress(r));
  std::atomic<size_t> next{0};
  std::atomic<uint64_t> ok{0}, ok_later{0}, lost{0}, errors{0};
  std::mutex out_mu;
  FILE* out = std::fopen(JoinPath(out_dir, "verify.csv").c_str(), "w");
  std::fprintf(out, "key,last_acked_seq,returned_seq,result,detail\n");
  std::vector<std::thread> ts;
  for (int t = 0; t < threads; ++t)
    ts.emplace_back([&] {
      KvClient client(routers, 3000);
      for (size_t i = next++; i < work.size(); i = next++) {
        const auto& key = work[i].first;
        const auto& st = work[i].second;
        GetResult r;
        for (int attempt = 0; attempt < 3; ++attempt) {  // retry transient errors, not data mismatches
          r = client.get(key);
          if (r.code == wire::OK || r.code == wire::NOT_FOUND) break;
        }
        std::string result, detail;
        uint64_t got = 0;
        if (r.code == wire::OK && ParseWorkloadSeq(key, r.value, &got)) {
          if (got == st.last_acked && r.value == WorkloadValue(key, got, value_bytes)) {
            result = "OK";
            ok++;
          } else if (st.maybe_after.count(got) && r.value == WorkloadValue(key, got, value_bytes)) {
            result = "OK_LATER_MAYBE";
            ok_later++;
          } else {
            result = "LOST";
            detail = got < st.last_acked ? "older value" : "unexpected value";
            lost++;
          }
        } else if (r.code == wire::OK) {
          result = "LOST";
          detail = "corrupt value";
          lost++;
        } else if (r.code == wire::NOT_FOUND) {
          result = "LOST";
          detail = "not found";
          lost++;
        } else {
          result = "ERROR";
          detail = PbCodeName(r.code);
          errors++;
        }
        std::lock_guard<std::mutex> l(out_mu);
        std::fprintf(out, "%s,%llu,%llu,%s,%s\n", key.c_str(), (unsigned long long)st.last_acked,
                     (unsigned long long)got, result.c_str(), detail.c_str());
      }
    });
  for (auto& t : ts) t.join();
  std::fclose(out);
  std::string summary = "{\"acked_keys\":" + std::to_string(work.size()) + ",\"ok\":" + std::to_string(ok.load()) +
                        ",\"ok_later_maybe\":" + std::to_string(ok_later.load()) + ",\"lost\":" +
                        std::to_string(lost.load()) + ",\"errors\":" + std::to_string(errors.load()) + "}\n";
  WriteFileAtomic(JoinPath(out_dir, "verify_summary.json"), summary);
  std::printf("verify: %zu acked keys: %llu OK, %llu OK (later unacked write survived), %llu LOST, %llu read errors\n",
              work.size(), (unsigned long long)ok.load(), (unsigned long long)ok_later.load(),
              (unsigned long long)lost.load(), (unsigned long long)errors.load());
  return (lost.load() == 0 && errors.load() == 0) ? 0 : 1;
}
