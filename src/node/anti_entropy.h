// Merkle-tree anti-entropy (LLD §7.6): startup repair pass + periodic background pass.
#pragma once

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/clock.h"
#include "common/event_log.h"
#include "merkle/merkle_tree.h"
#include "node/admin_reporter.h"
#include "node/failure_detector.h"
#include "rpc/peer_client_pool.h"
#include "storage/storage_engine.h"

namespace dkv {

struct RepairStats {
  uint64_t ranges_compared = 0;
  uint64_t ranges_differing = 0;
  uint64_t keys_fetched = 0;
  uint64_t versions_stored = 0;
  uint64_t bytes_fetched = 0;
  uint64_t rpcs = 0;
  bool ok = true;
  void add(const RepairStats& o);
};

class AntiEntropy {
 public:
  struct Options {
    uint32_t interval_s = 30;
    uint32_t coord_timeout_ms = 500;
    uint32_t rpc_timeout_ms = 5000;
    uint32_t wait_peers_max_ms = 10000;
  };

  AntiEntropy(std::string self, uint64_t incarnation, int64_t process_start_wall_ms, Options opts,
              MerkleForest& forest, StorageEngine& engine, PeerClientPool& peers, FailureDetector& fd,
              AdminReporter* admin, Clock& clock, EventLog* events);
  ~AntiEntropy();

  void start();  // startup pass, then periodic loop, on its own thread
  void stop();
  bool startup_done() const { return startup_done_.load(); }

  // Full Merkle comparison with one peer over the given ranges; pulls missing/newer versions.
  RepairStats sync_with(const std::string& peer, const std::vector<uint32_t>& ranges);

 private:
  void run();
  void startup_pass();
  std::map<std::string, std::vector<uint32_t>> shared_ranges() const;  // peer -> ranges both replicate
  bool wait_or_stop(int64_t ms);

  const std::string self_;
  const uint64_t incarnation_;
  const int64_t start_wall_ms_;
  const Options opts_;
  MerkleForest& forest_;
  StorageEngine& engine_;
  PeerClientPool& peers_;
  FailureDetector& fd_;
  AdminReporter* admin_;
  Clock& clock_;
  EventLog* events_;

  std::atomic<bool> startup_done_{false};
  std::mutex mu_;
  std::condition_variable cv_;
  bool stopping_ = false;
  std::thread thread_;
};

}  // namespace dkv
