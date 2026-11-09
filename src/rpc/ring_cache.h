// Cached ring for nodes and routers (LLD §5.2). Polls the Metadata Service by epoch; if Metadata is
// unreachable at startup, builds the same deterministic ring from the config (HLD §9).
#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "common/clock.h"
#include "common/status.h"
#include "config.pb.h"
#include "metadata.grpc.pb.h"
#include "ring/hash_ring.h"

namespace dkv {

class MetadataClient {
 public:
  virtual ~MetadataClient() = default;
  virtual StatusOr<wire::RingInfo> GetRing(uint64_t known_epoch, int64_t timeout_ms) = 0;
};

class GrpcMetadataClient final : public MetadataClient {
 public:
  explicit GrpcMetadataClient(const std::string& address);
  StatusOr<wire::RingInfo> GetRing(uint64_t known_epoch, int64_t timeout_ms) override;

 private:
  std::unique_ptr<wire::MetadataService::Stub> stub_;
};

class RingCache {
 public:
  RingCache(MetadataClient* client, const wire::ClusterConfig& cfg);
  ~RingCache();

  std::shared_ptr<const HashRing> current() const;
  void refresh_now();  // one synchronous GetRing attempt
  void start();        // background polling every ring_poll_interval_s
  void stop();

 private:
  void poll_loop();
  MetadataClient* client_;
  wire::ClusterConfig cfg_;
  mutable std::mutex mu_;
  std::shared_ptr<const HashRing> ring_;
  std::condition_variable cv_;
  bool stopping_ = false;
  std::thread thread_;
};

}  // namespace dkv
