// Coordinator role of a storage node: PUT and GET with quorums (LLD §7.1, §7.2).
#pragma once

#include <array>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

#include "common/clock.h"
#include "common/event_log.h"
#include "config.pb.h"
#include "kv.pb.h"
#include "node/failure_detector.h"
#include "rpc/peer_client_pool.h"
#include "rpc/ring_cache.h"
#include "storage/storage_engine.h"

namespace dkv {

// LRU of request_id -> PutResponse so a router retry of the same PUT is not applied twice.
class RequestDeduper {
 public:
  explicit RequestDeduper(size_t capacity = 100000) : capacity_(capacity) {}
  bool get(const std::string& id, wire::PutResponse* out);
  void put(const std::string& id, const wire::PutResponse& resp);

 private:
  size_t capacity_;
  std::mutex mu_;
  std::list<std::string> order_;  // front = newest
  std::unordered_map<std::string, std::pair<wire::PutResponse, std::list<std::string>::iterator>> map_;
};

class KeyLocks {
 public:
  std::mutex& for_key(const std::string& key) { return stripes_[std::hash<std::string>{}(key) % kStripes]; }

 private:
  static constexpr size_t kStripes = 1024;
  std::array<std::mutex, kStripes> stripes_;
};

class Coordinator {
 public:
  Coordinator(std::string self, const wire::ClusterConfig& cfg, RingCache& ring, FailureDetector& fd,
              StorageEngine& engine, PeerClientPool& peers, ThreadPool& pool, Clock& clock,
              EventLog* events);

  void put(const wire::PutRequest& req, wire::PutResponse* resp);
  void get(const wire::GetRequest& req, wire::GetResponse* resp);

  uint32_t effective_w(const wire::PutRequest&) const { return w_; }  // [P2] honour req.w()
  uint32_t effective_r(const wire::GetRequest&) const { return r_; }  // [P2] honour req.r()

 private:
  std::vector<std::string> live_peers(const std::vector<std::string>& pref) const;
  void read_repair(const std::string& key, const wire::Version& winner,
                   const std::vector<std::string>& stale_peers, bool repair_self);

  const std::string self_;
  const uint32_t n_, r_, w_;
  const int64_t timeout_ms_;
  const bool read_before_write_;
  RingCache& ring_;
  FailureDetector& fd_;
  StorageEngine& engine_;
  PeerClientPool& peers_;
  ThreadPool& pool_;
  Clock& clock_;
  EventLog* events_;
  VersionResolver resolver_;
  KeyLocks locks_;
  RequestDeduper dedupe_;
};

}  // namespace dkv
