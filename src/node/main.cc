// dkv_node --config <cluster.json> --id <A>
#include <cstdio>

#include "common/config_loader.h"
#include "common/flags.h"
#include "common/logging.h"
#include "common/signals.h"
#include "node/storage_node.h"

int main(int argc, char** argv) {
  dkv::BlockTerminationSignals();
  dkv::Flags flags(argc, argv);
  std::string id = flags.get("id");
  if (!flags.has("config") || id.empty()) {
    std::fprintf(stderr, "usage: dkv_node --config <cluster.json> --id <node-id> [--log debug]\n");
    return 2;
  }
  if (flags.get("log") == "debug") dkv::SetLogLevel(dkv::LogLevel::DEBUG);
  dkv::SetLogPrefix("node-" + id);
  auto cfg = dkv::LoadClusterConfig(flags.get("config"));
  if (!cfg.ok()) {
    DKV_LOG(ERROR) << cfg.status().ToString();
    return 1;
  }
  dkv::StorageNode node(*cfg, id);
  dkv::Status st = node.start();
  if (!st.ok()) {
    DKV_LOG(ERROR) << "startup failed: " << st.ToString();
    return 1;
  }
  int sig = dkv::WaitForTerminationSignal();  // main thread owns shutdown: no concurrent stop/destroy
  DKV_LOG(INFO) << "signal " << sig << ": shutting down";
  node.stop();
  return 0;
}
