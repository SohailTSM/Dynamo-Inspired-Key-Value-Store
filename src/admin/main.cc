// dkv_admin --config <cluster.json> [--out <dir>]
// Auto-Admin (LLD §5.8): cluster health view, DOWN/UP decisions with peer confirmation, repair tracking.
#include <atomic>
#include <cstdio>

#include "admin.grpc.pb.h"
#include "admin/health_tracker.h"
#include "common/config_loader.h"
#include "common/flags.h"
#include "common/fs.h"
#include "common/logging.h"
#include "common/signals.h"
#include "rpc/rpc_util.h"

namespace dkv {

class AdminServiceImpl final : public wire::AdminService::Service {
 public:
  explicit AdminServiceImpl(HealthTracker& t) : t_(t) {}
  void shutdown() { stopping_ = true; }

  grpc::Status Heartbeat(grpc::ServerContext*, const wire::HeartbeatMsg* m, wire::HeartbeatAck* ack) override {
    t_.on_heartbeat(*m);
    ack->set_from("admin");
    return grpc::Status::OK;
  }
  grpc::Status Register(grpc::ServerContext*, const wire::RegisterRequest* r, wire::Result* res) override {
    t_.on_register(*r);
    SetResult(res, wire::OK);
    return grpc::Status::OK;
  }
  grpc::Status ReportRepair(grpc::ServerContext*, const wire::RepairReport* r, wire::Result* res) override {
    t_.on_repair_report(*r);
    SetResult(res, wire::OK);
    return grpc::Status::OK;
  }
  grpc::Status WatchHealth(grpc::ServerContext* ctx, const wire::Empty*,
                           grpc::ServerWriter<wire::HealthSnapshot>* writer) override {
    wire::HealthSnapshot snap = t_.snapshot();
    while (!ctx->IsCancelled() && !stopping_) {
      if (!writer->Write(snap)) break;
      snap = t_.wait_for_change(snap.version(), 1000);  // also a 1 s keep-alive
    }
    return grpc::Status::OK;
  }
  grpc::Status GetClusterStatus(grpc::ServerContext*, const wire::Empty*, wire::ClusterStatus* st) override {
    *st = t_.status();
    return grpc::Status::OK;
  }

 private:
  HealthTracker& t_;
  std::atomic<bool> stopping_{false};
};

}  // namespace dkv

int main(int argc, char** argv) {
  dkv::BlockTerminationSignals();
  dkv::Flags flags(argc, argv);
  if (!flags.has("config")) {
    std::fprintf(stderr, "usage: dkv_admin --config <cluster.json> [--out <dir>]\n");
    return 2;
  }
  dkv::SetLogPrefix("admin");
  auto cfg = dkv::LoadClusterConfig(flags.get("config"));
  if (!cfg.ok()) {
    DKV_LOG(ERROR) << cfg.status().ToString();
    return 1;
  }
  std::string out = flags.get("out", dkv::NodeDataDir(*cfg, "admin"));
  dkv::EventLog events;
  auto& clock = dkv::SystemClock::Instance();
  if (auto st = events.open(dkv::JoinPath(out, "events.jsonl"), "admin", &clock); !st.ok()) {
    DKV_LOG(ERROR) << st.ToString();
    return 1;
  }
  events.emit(dkv::Ev("proc.start").i64("start_wall_ms", clock.now_wall_ms()));
  std::map<std::string, std::string> nodes;
  for (const auto& e : cfg->storage_nodes()) nodes[e.id()] = dkv::EndpointAddress(e);
  dkv::HealthTracker::Options o;
  o.interval_ms = cfg->timing().heartbeat_interval_ms();
  o.suspect_after = cfg->timing().suspect_after_missed();
  o.down_after = cfg->timing().down_after_missed();
  o.tick_ms = cfg->timing().detector_tick_ms();
  dkv::HealthTracker tracker(nodes, o, clock, &events, out);
  tracker.start();
  dkv::AdminServiceImpl svc(tracker);
  std::string err;
  auto server = dkv::StartServer(cfg->admin().port(), {&svc}, 64, &err);
  if (!server) {
    DKV_LOG(ERROR) << err;
    return 1;
  }
  DKV_LOG(INFO) << "auto-admin on port " << cfg->admin().port() << ", tracking " << nodes.size()
                << " nodes, output " << out;
  dkv::WaitForTerminationSignal();
  svc.shutdown();  // ends WatchHealth streams (they wake at least once per second)
  server->Shutdown(std::chrono::system_clock::now() + std::chrono::milliseconds(1500));
  tracker.stop();
  events.close();
  return 0;
}
