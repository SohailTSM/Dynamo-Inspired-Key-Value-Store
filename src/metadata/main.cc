// dkv_metadata --config <cluster.json>
// Metadata Service (LLD §5.7): source of truth for membership + ring tokens + quorum defaults.
// Phase 1 membership is static: epoch is fixed at 1 for the life of the process.
#include <google/protobuf/util/json_util.h>

#include <cstdio>
#include <mutex>

#include "common/config_loader.h"
#include "common/fs.h"
#include "common/flags.h"
#include "common/logging.h"
#include "common/signals.h"
#include "metadata.grpc.pb.h"
#include "ring/hash_ring.h"
#include "rpc/rpc_util.h"

namespace dkv {

class MetadataServiceImpl final : public wire::MetadataService::Service {
 public:
  explicit MetadataServiceImpl(wire::RingInfo info) : info_(std::move(info)) {}
  grpc::Status GetRing(grpc::ServerContext*, const wire::GetRingRequest* req, wire::RingInfo* resp) override {
    std::lock_guard<std::mutex> l(mu_);
    if (req->known_epoch() == info_.epoch()) {
      resp->set_epoch(info_.epoch());
      resp->set_unchanged(true);
      return grpc::Status::OK;
    }
    *resp = info_;
    return grpc::Status::OK;
  }

 private:
  std::mutex mu_;
  wire::RingInfo info_;
};

}  // namespace dkv

int main(int argc, char** argv) {
  dkv::BlockTerminationSignals();
  dkv::Flags flags(argc, argv);
  if (!flags.has("config")) {
    std::fprintf(stderr, "usage: dkv_metadata --config <cluster.json>\n");
    return 2;
  }
  dkv::SetLogPrefix("metadata");
  auto cfg = dkv::LoadClusterConfig(flags.get("config"));
  if (!cfg.ok()) {
    DKV_LOG(ERROR) << cfg.status().ToString();
    return 1;
  }
  dkv::wire::RingInfo info = dkv::HashRing::FromConfig(*cfg, /*epoch=*/1).ToRingInfo();
  info.mutable_quorum()->CopyFrom(cfg->quorum());

  // Persist the ring for inspection / restart (write-temp + fsync + rename).
  std::string dir = dkv::NodeDataDir(*cfg, "metadata");
  std::string json;
  google::protobuf::util::JsonPrintOptions jo;
  jo.add_whitespace = true;
  (void)google::protobuf::util::MessageToJsonString(info, &json, jo);
  if (dkv::MakeDirs(dir).ok()) {
    auto st = dkv::WriteFileAtomic(dkv::JoinPath(dir, "ring.json"), json);
    if (!st.ok()) DKV_LOG(WARN) << "could not persist ring: " << st.ToString();
  }

  dkv::MetadataServiceImpl svc(info);
  std::string err;
  auto server = dkv::StartServer(cfg->metadata().port(), {&svc}, 32, &err);
  if (!server) {
    DKV_LOG(ERROR) << err;
    return 1;
  }
  DKV_LOG(INFO) << "metadata service on port " << cfg->metadata().port() << ": " << info.nodes_size()
                << " nodes, " << cfg->vnodes_per_node() << " vnodes each, epoch " << info.epoch();
  dkv::WaitForTerminationSignal();
  server->Shutdown(std::chrono::system_clock::now() + std::chrono::milliseconds(500));
  return 0;
}
