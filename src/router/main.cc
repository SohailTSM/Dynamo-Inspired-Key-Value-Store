// dkv_router --config <cluster.json> --id <R1>
#include <cstdio>

#include "common/config_loader.h"
#include "common/flags.h"
#include "common/logging.h"
#include "common/signals.h"
#include "router/router.h"
#include "rpc/rpc_util.h"

int main(int argc, char** argv) {
  dkv::BlockTerminationSignals();
  dkv::Flags flags(argc, argv);
  std::string id = flags.get("id", "R1");
  if (!flags.has("config")) {
    std::fprintf(stderr, "usage: dkv_router --config <cluster.json> [--id R1]\n");
    return 2;
  }
  dkv::SetLogPrefix("router-" + id);
  auto cfg = dkv::LoadClusterConfig(flags.get("config"));
  if (!cfg.ok()) {
    DKV_LOG(ERROR) << cfg.status().ToString();
    return 1;
  }
  uint32_t port = 0;
  for (const auto& r : cfg->routers())
    if (r.id() == id) port = r.port();
  if (port == 0) {
    DKV_LOG(ERROR) << "router id " << id << " not in config";
    return 1;
  }
  auto& clock = dkv::SystemClock::Instance();
  std::unique_ptr<dkv::GrpcMetadataClient> meta;
  if (!cfg->metadata().host().empty())
    meta = std::make_unique<dkv::GrpcMetadataClient>(dkv::EndpointAddress(cfg->metadata()));
  dkv::RingCache ring(meta.get(), *cfg);
  ring.start();
  dkv::HealthView health(cfg->admin().host().empty() ? "" : dkv::EndpointAddress(cfg->admin()), clock);
  health.start();
  dkv::RouterServiceImpl svc(*cfg, ring, health, clock);
  std::string err;
  auto server = dkv::StartServer(port, {&svc}, 256, &err);
  if (!server) {
    DKV_LOG(ERROR) << err;
    return 1;
  }
  DKV_LOG(INFO) << "router " << id << " on port " << port << " (ring epoch " << ring.current()->epoch() << ")";
  dkv::WaitForTerminationSignal();
  health.stop();
  server->Shutdown(std::chrono::system_clock::now() + std::chrono::milliseconds(500));
  ring.stop();
  return 0;
}
