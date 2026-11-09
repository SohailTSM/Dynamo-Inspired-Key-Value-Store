#include "rpc/ring_cache.h"

#include "common/logging.h"
#include "rpc/rpc_util.h"

namespace dkv {

GrpcMetadataClient::GrpcMetadataClient(const std::string& address)
    : stub_(wire::MetadataService::NewStub(MakeChannel(address))) {}

StatusOr<wire::RingInfo> GrpcMetadataClient::GetRing(uint64_t known_epoch, int64_t timeout_ms) {
  grpc::ClientContext ctx;
  SetDeadlineMs(&ctx, timeout_ms);
  wire::GetRingRequest req;
  req.set_known_epoch(known_epoch);
  wire::RingInfo info;
  grpc::Status st = stub_->GetRing(&ctx, req, &info);
  if (!st.ok()) return Status::Unavailable("metadata GetRing: " + st.error_message());
  return info;
}

RingCache::RingCache(MetadataClient* client, const wire::ClusterConfig& cfg) : client_(client), cfg_(cfg) {
  ring_ = std::make_shared<const HashRing>(HashRing::FromConfig(cfg_, 0));
}

RingCache::~RingCache() { stop(); }

std::shared_ptr<const HashRing> RingCache::current() const {
  std::lock_guard<std::mutex> l(mu_);
  return ring_;
}

void RingCache::refresh_now() {
  if (!client_) return;
  uint64_t known = current()->epoch();
  auto info = client_->GetRing(known, 1000);
  if (!info.ok()) {
    DKV_LOG(DEBUG) << "ring refresh failed, keeping epoch " << known << ": " << info.status().ToString();
    return;
  }
  if (info->unchanged() || info->nodes_size() == 0) return;
  auto ring = std::make_shared<const HashRing>(HashRing::FromRingInfo(*info));
  std::lock_guard<std::mutex> l(mu_);
  if (ring->epoch() != ring_->epoch())
    DKV_LOG(INFO) << "ring updated: epoch " << ring_->epoch() << " -> " << ring->epoch();
  ring_ = std::move(ring);
}

void RingCache::start() {
  refresh_now();
  if (!client_) return;
  thread_ = std::thread([this] { poll_loop(); });
}

void RingCache::stop() {
  {
    std::lock_guard<std::mutex> l(mu_);
    stopping_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void RingCache::poll_loop() {
  auto interval = std::chrono::seconds(cfg_.timing().ring_poll_interval_s());
  for (;;) {
    {
      std::unique_lock<std::mutex> l(mu_);
      if (cv_.wait_for(l, interval, [this] { return stopping_; })) return;
    }
    refresh_now();
  }
}

}  // namespace dkv
