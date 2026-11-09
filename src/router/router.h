// Request Router (LLD §5.6, §7.9): stateless front door. Cached ring + health view, coordinator
// selection (first non-DOWN node in the preference list), one failover on transport errors.
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "admin.grpc.pb.h"
#include "common/clock.h"
#include "kv.grpc.pb.h"
#include "node.grpc.pb.h"
#include "rpc/ring_cache.h"

namespace dkv {

class HealthView {
 public:
  HealthView(std::string admin_address, Clock& clock);
  ~HealthView();
  void start();
  void stop();
  bool is_down(const std::string& node) const;
  void mark_local_down(const std::string& node, int64_t ms);

 private:
  void watch_loop();
  std::string admin_address_;
  Clock& clock_;
  mutable std::mutex mu_;
  std::map<std::string, wire::NodeHealth> health_;
  std::map<std::string, int64_t> local_down_until_;
  grpc::ClientContext* active_ctx_ = nullptr;  // guarded by mu_, for cancellation
  std::atomic<bool> stopping_{false};
  std::thread thread_;
};

class RouterServiceImpl final : public wire::KvService::Service {
 public:
  RouterServiceImpl(const wire::ClusterConfig& cfg, RingCache& ring, HealthView& health, Clock& clock);

  grpc::Status Put(grpc::ServerContext*, const wire::PutRequest* req, wire::PutResponse* resp) override;
  grpc::Status Get(grpc::ServerContext*, const wire::GetRequest* req, wire::GetResponse* resp) override;

 private:
  wire::CoordinatorService::Stub* stub(const std::string& node);
  // First node of the preference list that is not DOWN and not excluded; "" if none.
  std::string select(const std::string& key, const std::vector<std::string>& exclude) const;
  template <typename Req, typename Resp>
  void forward(const Req& req, Resp* resp,
               grpc::Status (wire::CoordinatorService::Stub::*m)(grpc::ClientContext*, const Req&, Resp*));

  const int64_t timeout_ms_;
  RingCache& ring_;
  HealthView& health_;
  Clock& clock_;
  std::map<std::string, std::string> addresses_;
  std::mutex mu_;
  std::map<std::string, std::unique_ptr<wire::CoordinatorService::Stub>> stubs_;
};

Status ValidateKeyValue(const std::string& key, const std::string* value);

}  // namespace dkv
