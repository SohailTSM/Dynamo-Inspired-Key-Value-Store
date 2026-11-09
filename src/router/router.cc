#include "router/router.h"

#include <algorithm>

#include "common/config_loader.h"
#include "common/fs.h"
#include "common/logging.h"
#include "rpc/rpc_util.h"

namespace dkv {

constexpr size_t kMaxKeyBytes = 1024;
constexpr size_t kMaxValueBytes = 1 << 20;

Status ValidateKeyValue(const std::string& key, const std::string* value) {
  if (key.empty() || key.size() > kMaxKeyBytes) return Status::InvalidArgument("key must be 1..1024 bytes");
  if (value && value->size() > kMaxValueBytes) return Status::InvalidArgument("value larger than 1 MiB");
  return Status::OK();
}

// ------------------------------------------------------------------------------------ HealthView

HealthView::HealthView(std::string admin_address, Clock& clock)
    : admin_address_(std::move(admin_address)), clock_(clock) {}

HealthView::~HealthView() { stop(); }

void HealthView::start() {
  if (admin_address_.empty()) return;
  thread_ = std::thread([this] { watch_loop(); });
}

void HealthView::stop() {
  stopping_ = true;
  {
    std::lock_guard<std::mutex> l(mu_);
    if (active_ctx_) active_ctx_->TryCancel();
  }
  if (thread_.joinable()) thread_.join();
}

void HealthView::watch_loop() {
  auto stub = wire::AdminService::NewStub(MakeChannel(admin_address_));
  while (!stopping_) {
    grpc::ClientContext ctx;
    {
      std::lock_guard<std::mutex> l(mu_);
      active_ctx_ = &ctx;
    }
    wire::Empty req;
    auto reader = stub->WatchHealth(&ctx, req);
    wire::HealthSnapshot snap;
    while (!stopping_ && reader->Read(&snap)) {
      std::lock_guard<std::mutex> l(mu_);
      for (const auto& n : snap.nodes()) {
        auto it = health_.find(n.node());
        if (it == health_.end() || it->second != n.health())
          DKV_LOG(INFO) << "health view: " << n.node() << " = " << wire::NodeHealth_Name(n.health());
        health_[n.node()] = n.health();
      }
    }
    reader->Finish();
    {
      std::lock_guard<std::mutex> l(mu_);
      active_ctx_ = nullptr;
    }
    // Admin unreachable: keep the last snapshot (HLD §9) and retry.
    for (int i = 0; i < 5 && !stopping_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

bool HealthView::is_down(const std::string& node) const {
  std::lock_guard<std::mutex> l(mu_);
  auto ld = local_down_until_.find(node);
  if (ld != local_down_until_.end() && ld->second > clock_.now_mono_ms()) return true;
  auto it = health_.find(node);
  return it != health_.end() && it->second == wire::DOWN;
}

void HealthView::mark_local_down(const std::string& node, int64_t ms) {
  std::lock_guard<std::mutex> l(mu_);
  local_down_until_[node] = clock_.now_mono_ms() + ms;
}

// ----------------------------------------------------------------------------- RouterServiceImpl

RouterServiceImpl::RouterServiceImpl(const wire::ClusterConfig& cfg, RingCache& ring, HealthView& health,
                                     Clock& clock)
    : timeout_ms_(cfg.timing().router_timeout_ms()), ring_(ring), health_(health), clock_(clock) {
  for (const auto& e : cfg.storage_nodes()) addresses_[e.id()] = EndpointAddress(e);
}

wire::CoordinatorService::Stub* RouterServiceImpl::stub(const std::string& node) {
  std::lock_guard<std::mutex> l(mu_);
  auto it = stubs_.find(node);
  if (it != stubs_.end()) return it->second.get();
  auto a = addresses_.find(node);
  if (a == addresses_.end()) return nullptr;
  auto s = wire::CoordinatorService::NewStub(MakeChannel(a->second));
  auto* raw = s.get();
  stubs_[node] = std::move(s);
  return raw;
}

std::string RouterServiceImpl::select(const std::string& key, const std::vector<std::string>& exclude) const {
  auto pref = ring_.current()->preference_list(key);
  auto excluded = [&](const std::string& n) { return std::find(exclude.begin(), exclude.end(), n) != exclude.end(); };
  for (const auto& n : pref)
    if (!excluded(n) && !health_.is_down(n)) return n;
  // Every replica looks down: try the first non-excluded one anyway (our view may be stale).
  for (const auto& n : pref)
    if (!excluded(n)) return n;
  return "";
}

template <typename Req, typename Resp>
void RouterServiceImpl::forward(const Req& req, Resp* resp,
                                grpc::Status (wire::CoordinatorService::Stub::*m)(grpc::ClientContext*, const Req&,
                                                                                 Resp*)) {
  std::vector<std::string> exclude;
  std::string last_err = "no live replica";
  wire::Code last_code = wire::UNAVAILABLE;
  for (int attempt = 0; attempt < 2; ++attempt) {
    std::string c = select(req.key(), exclude);
    if (c.empty()) break;
    auto* s = stub(c);
    if (!s) break;
    grpc::ClientContext ctx;
    SetDeadlineMs(&ctx, timeout_ms_);
    Resp r;
    grpc::Status st = (s->*m)(&ctx, req, &r);
    if (st.ok() && r.result().code() != wire::NOT_A_REPLICA) {
      *resp = r;
      return;
    }
    if (st.ok()) {  // NOT_A_REPLICA: our ring is stale
      ring_.refresh_now();
      last_err = "coordinator " + c + " is not a replica";
    } else {
      health_.mark_local_down(c, 2000);
      last_code = st.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED ? wire::TIMEOUT : wire::UNAVAILABLE;
      last_err = "coordinator " + c + ": " + st.error_message();
    }
    exclude.push_back(c);
  }
  SetResult(resp->mutable_result(), last_code, last_err);
}

grpc::Status RouterServiceImpl::Put(grpc::ServerContext*, const wire::PutRequest* req, wire::PutResponse* resp) {
  Status v = ValidateKeyValue(req->key(), &req->value());
  if (!v.ok()) {
    SetResult(resp->mutable_result(), wire::INVALID_ARGUMENT, v.message());
    return grpc::Status::OK;
  }
  if (req->request_id().empty()) {
    wire::PutRequest copy = *req;
    copy.set_request_id(RandomId());
    forward(copy, resp, &wire::CoordinatorService::Stub::CoordPut);
  } else {
    forward(*req, resp, &wire::CoordinatorService::Stub::CoordPut);
  }
  return grpc::Status::OK;
}

grpc::Status RouterServiceImpl::Get(grpc::ServerContext*, const wire::GetRequest* req, wire::GetResponse* resp) {
  Status v = ValidateKeyValue(req->key(), nullptr);
  if (!v.ok()) {
    SetResult(resp->mutable_result(), wire::INVALID_ARGUMENT, v.message());
    return grpc::Status::OK;
  }
  forward(*req, resp, &wire::CoordinatorService::Stub::CoordGet);
  return grpc::Status::OK;
}

}  // namespace dkv
