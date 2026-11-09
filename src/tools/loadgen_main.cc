// dkv_loadgen --config <cluster.json> --out <dir> [--rate 500] [--threads 16] [--keys 10000]
//             [--duration 60] [--put-ratio 0.5] [--value-bytes 256] [--preload] [--prefix k] [--seq-base N]
//             [--run-until-file <path>]   (stop early when this file appears)
//
// Open-loop workload (LLD §5.9). Each worker owns the keys with index % threads == worker, so PUTs to
// one key are sequential from the client side (needed by the verifier, LLD §7.8).
// Output per worker:  requests_<w>.csv  and  acks_<w>.csv ; plus summary.json.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <random>
#include <thread>
#include <vector>

#include "client/kv_client.h"
#include "common/config_loader.h"
#include "common/fs.h"
#include "common/flags.h"
#include "common/signals.h"
#include "rpc/rpc_util.h"
#include "tools/workload.h"

using namespace dkv;
using Clock = std::chrono::steady_clock;

namespace {

int64_t WallMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
int64_t MonoUs() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

struct Totals {
  std::atomic<uint64_t> ops{0}, ok{0}, puts{0}, gets{0}, put_ok{0}, get_ok{0}, not_found{0};
  std::atomic<uint64_t> by_code[8] = {};
};

}  // namespace

int main(int argc, char** argv) {
  BlockTerminationSignals();  // SIGTERM/SIGINT -> stop cleanly and flush logs (used by RCE experiment)
  Flags f(argc, argv);
  if (!f.has("config") || !f.has("out")) {
    std::fprintf(stderr, "usage: dkv_loadgen --config <cluster.json> --out <dir> [--rate N] [--threads N] "
                         "[--keys N] [--duration S] [--put-ratio X] [--value-bytes N] [--preload] [--prefix P]\n");
    return 2;
  }
  auto cfg = LoadClusterConfig(f.get("config"));
  if (!cfg.ok()) {
    std::fprintf(stderr, "%s\n", cfg.status().ToString().c_str());
    return 1;
  }
  const std::string out = f.get("out");
  const double rate = f.get_double("rate", 500);
  const int threads = int(f.get_int("threads", 16));
  const uint64_t keys = uint64_t(f.get_int("keys", 10000));
  const double duration = f.get_double("duration", 60);
  const double put_ratio = f.get_double("put-ratio", 0.5);
  const uint32_t value_bytes = uint32_t(f.get_int("value-bytes", 256));
  const bool preload = f.get_bool("preload", false);
  const std::string prefix = f.get("prefix", "k");
  const std::string stop_file = f.get("run-until-file", "");
  const uint64_t seq_base = uint64_t(f.get_int("seq-base", 0));  // keeps seqs distinct across runs
  if (!MakeDirs(out).ok()) {
    std::fprintf(stderr, "cannot create %s\n", out.c_str());
    return 1;
  }
  std::vector<std::string> routers;
  for (const auto& r : cfg->routers()) routers.push_back(EndpointAddress(r));

  Totals tot;
  std::atomic<bool> stop{false};
  OnTerminationSignal([&stop] { stop = true; });
  const int64_t start_wall = WallMs();
  std::vector<std::thread> workers;
  for (int w = 0; w < threads; ++w) {
    workers.emplace_back([&, w] {
      KvClient client(routers, 2000);
      std::string rp = JoinPath(out, "requests_" + std::to_string(w) + ".csv");
      std::string ap = JoinPath(out, "acks_" + std::to_string(w) + ".csv");
      FILE* req = std::fopen(rp.c_str(), "w");
      FILE* ack = std::fopen(ap.c_str(), "w");
      std::fprintf(req, "t_start_wall_ms,t_start_mono_us,latency_us,op,key,code,coordinator\n");
      std::fprintf(ack, "worker,key,seq,status,t_wall_ms\n");
      std::vector<uint64_t> owned;
      for (uint64_t i = uint64_t(w); i < keys; i += uint64_t(threads)) owned.push_back(i);
      std::map<uint64_t, uint64_t> seq;  // key index -> last issued seq (starts at seq_base)
      std::mt19937_64 rng(uint64_t(w) * 7919 + 17);

      auto do_put = [&](uint64_t ki) {
        std::string key = WorkloadKey(prefix, ki);
        auto it = seq.find(ki);
        if (it == seq.end()) it = seq.emplace(ki, seq_base).first;
        uint64_t s = ++it->second;
        int64_t t_wall = WallMs(), t_mono = MonoUs();
        PutResult r = client.put(key, WorkloadValue(key, s, value_bytes));
        std::fprintf(req, "%lld,%lld,%lld,PUT,%s,%s,%s\n", (long long)t_wall, (long long)t_mono,
                     (long long)r.latency_us, key.c_str(), PbCodeName(r.code), r.coordinator.c_str());
        std::fprintf(ack, "%d,%s,%llu,%s,%lld\n", w, key.c_str(), (unsigned long long)s,
                     r.code == wire::OK ? "acked" : "maybe", (long long)WallMs());
        tot.ops++;
        tot.puts++;
        tot.by_code[std::min<int>(r.code, 7)]++;
        if (r.code == wire::OK) {
          tot.ok++;
          tot.put_ok++;
        }
      };

      if (preload)
        for (uint64_t ki : owned) do_put(ki);

      if (owned.empty() || rate <= 0) {
        std::fclose(req);
        std::fclose(ack);
        return;
      }
      const double per_worker = rate / threads;
      const auto interval = std::chrono::duration<double>(1.0 / per_worker);
      const auto t0 = Clock::now();
      const auto t_end = t0 + std::chrono::duration<double>(duration);
      std::uniform_int_distribution<size_t> pick(0, owned.size() - 1);
      std::uniform_real_distribution<double> coin(0, 1);
      for (uint64_t i = 0; !stop; ++i) {
        auto due = t0 + std::chrono::duration_cast<Clock::duration>(interval * double(i));
        if (due >= t_end) break;
        std::this_thread::sleep_until(due);  // open loop: if behind, issue immediately
        uint64_t ki = owned[pick(rng)];
        if (coin(rng) < put_ratio) {
          do_put(ki);
        } else {
          std::string key = WorkloadKey(prefix, ki);
          int64_t t_wall = WallMs(), t_mono = MonoUs();
          GetResult r = client.get(key);
          std::fprintf(req, "%lld,%lld,%lld,GET,%s,%s,%s\n", (long long)t_wall, (long long)t_mono,
                       (long long)r.latency_us, key.c_str(), PbCodeName(r.code), r.coordinator.c_str());
          tot.ops++;
          tot.gets++;
          tot.by_code[std::min<int>(r.code, 7)]++;
          if (r.code == wire::OK || r.code == wire::NOT_FOUND) {
            tot.ok++;
            tot.get_ok++;
            if (r.code == wire::NOT_FOUND) tot.not_found++;
          }
        }
      }
      std::fclose(req);
      std::fclose(ack);
    });
  }
  std::thread watcher;
  if (!stop_file.empty())
    watcher = std::thread([&] {
      while (!stop) {
        if (FileExists(stop_file)) stop = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });
  for (auto& t : workers) t.join();
  stop = true;
  if (watcher.joinable()) watcher.join();

  const int64_t end_wall = WallMs();
  double secs = (end_wall - start_wall) / 1000.0;
  std::string summary = "{\"start_wall_ms\":" + std::to_string(start_wall) + ",\"end_wall_ms\":" +
                        std::to_string(end_wall) + ",\"ops\":" + std::to_string(tot.ops.load()) +
                        ",\"ok\":" + std::to_string(tot.ok.load()) + ",\"puts\":" + std::to_string(tot.puts.load()) +
                        ",\"put_ok\":" + std::to_string(tot.put_ok.load()) + ",\"gets\":" +
                        std::to_string(tot.gets.load()) + ",\"get_ok\":" + std::to_string(tot.get_ok.load()) +
                        ",\"not_found\":" + std::to_string(tot.not_found.load()) + ",\"codes\":{";
  for (int c = 0; c < 8; ++c)
    summary += std::string(c ? "," : "") + "\"" + PbCodeName(wire::Code(c)) + "\":" + std::to_string(tot.by_code[c].load());
  summary += "},\"rate\":" + std::to_string(rate) + ",\"threads\":" + std::to_string(threads) +
             ",\"keys\":" + std::to_string(keys) + ",\"put_ratio\":" + std::to_string(put_ratio) +
             ",\"value_bytes\":" + std::to_string(value_bytes) + "}\n";
  WriteFileAtomic(JoinPath(out, "summary.json"), summary);
  std::printf("loadgen: %llu ops in %.1fs (%.0f ops/s), success %.3f%% (puts %llu/%llu, gets %llu/%llu)\n",
              (unsigned long long)tot.ops.load(), secs, tot.ops.load() / std::max(secs, 0.001),
              tot.ops ? 100.0 * tot.ok.load() / tot.ops.load() : 100.0, (unsigned long long)tot.put_ok.load(),
              (unsigned long long)tot.puts.load(), (unsigned long long)tot.get_ok.load(),
              (unsigned long long)tot.gets.load());
  return 0;
}
