// Storage node composition root (LLD §5.5, §7.7).
#pragma once

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <memory>
#include <string>

#include "common/clock.h"
#include "common/event_log.h"
#include "common/thread_pool.h"
#include "config.pb.h"
#include "merkle/merkle_tree.h"
#include "node/admin_reporter.h"
#include "node/anti_entropy.h"
#include "node/coordinator.h"
#include "node/failure_detector.h"
#include "node/services.h"
#include "rpc/fault_injector.h"
#include "rpc/peer_client_pool.h"
#include "rpc/ring_cache.h"
#include "storage/storage_engine.h"

namespace dkv {

class StorageNode {
 public:
  StorageNode(wire::ClusterConfig cfg, std::string id);
  ~StorageNode();
  Status start();  // full startup sequence; returns once the server is serving
  void wait();     // blocks until the server shuts down
  void stop();
  uint64_t incarnation() const { return incarnation_; }

 private:
  Status bump_incarnation();

  wire::ClusterConfig cfg_;
  const std::string id_;
  std::string data_dir_;
  uint64_t incarnation_ = 0;
  int64_t start_wall_ms_ = 0;
  Clock& clock_ = SystemClock::Instance();

  EventLog events_;
  std::unique_ptr<LogStorageEngine> engine_;
  MerkleForest forest_;
  std::unique_ptr<GrpcMetadataClient> meta_client_;
  std::unique_ptr<RingCache> ring_;
  std::unique_ptr<ThreadPool> pool_;
  FaultInjector faults_;
  std::unique_ptr<PeerClientPool> peers_;
  std::unique_ptr<FailureDetector> fd_;
  std::unique_ptr<AdminReporter> admin_;
  std::unique_ptr<Coordinator> coordinator_;
  std::unique_ptr<CoordinatorServiceImpl> coord_svc_;
  std::unique_ptr<ReplicaServiceImpl> replica_svc_;
  std::unique_ptr<AntiEntropy> anti_entropy_;
  std::unique_ptr<grpc::Server> server_;
  std::atomic<bool> stopped_{false};
};

}  // namespace dkv
