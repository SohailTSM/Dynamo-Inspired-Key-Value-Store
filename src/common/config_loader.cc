#include "common/config_loader.h"

#include <google/protobuf/util/json_util.h>

#include <set>

#include "common/fs.h"

namespace dkv {

void ApplyConfigDefaults(wire::ClusterConfig* cfg) {
  auto* q = cfg->mutable_quorum();
  if (q->n() == 0) q->set_n(3);
  if (q->r() == 0) q->set_r(2);
  if (q->w() == 0) q->set_w(2);
  if (cfg->vnodes_per_node() == 0) cfg->set_vnodes_per_node(64);
  auto* t = cfg->mutable_timing();
  if (t->heartbeat_interval_ms() == 0) t->set_heartbeat_interval_ms(1000);
  if (t->suspect_after_missed() == 0) t->set_suspect_after_missed(1);
  if (t->down_after_missed() == 0) t->set_down_after_missed(3);
  if (t->detector_tick_ms() == 0) t->set_detector_tick_ms(100);
  if (t->coord_timeout_ms() == 0) t->set_coord_timeout_ms(500);
  if (t->router_timeout_ms() == 0) t->set_router_timeout_ms(1000);
  if (t->anti_entropy_interval_s() == 0) t->set_anti_entropy_interval_s(30);
  if (t->ring_poll_interval_s() == 0) t->set_ring_poll_interval_s(5);
  auto* s = cfg->mutable_storage();
  if (s->data_dir().empty()) s->set_data_dir("/tmp/${USER}/dynamo-kv");
  if (s->segment_bytes() == 0) s->set_segment_bytes(64ull << 20);
  if (cfg->cluster_name().empty()) cfg->set_cluster_name("dkv");
}

Status ValidateClusterConfig(const wire::ClusterConfig& cfg) {
  const auto& q = cfg.quorum();
  int nodes = cfg.storage_nodes_size();
  if (nodes < 1) return Status::InvalidArgument("config: no storage_nodes");
  if (q.n() < 1 || int(q.n()) > nodes)
    return Status::InvalidArgument("config: quorum.n must be in 1..number of storage nodes");
  if (q.r() < 1 || q.r() > q.n() || q.w() < 1 || q.w() > q.n())
    return Status::InvalidArgument("config: quorum r and w must be in 1..n");
  // Phase 1 requires overlapping quorums (HLD §3). [P2] relaxes this.
  if (q.r() + q.w() <= q.n())
    return Status::InvalidArgument(
        "config: Phase 1 requires r + w > n (overlapping quorums guarantee no lost acked writes)");
  std::set<std::string> ids;
  for (const auto& e : cfg.storage_nodes()) {
    if (e.id().empty() || e.host().empty() || e.port() == 0)
      return Status::InvalidArgument("config: storage node needs id, host, port");
    if (!ids.insert(e.id()).second)
      return Status::InvalidArgument("config: duplicate storage node id " + e.id());
  }
  if (cfg.timing().down_after_missed() < cfg.timing().suspect_after_missed())
    return Status::InvalidArgument("config: down_after_missed < suspect_after_missed");
  return Status::OK();
}

StatusOr<wire::ClusterConfig> ParseClusterConfig(const std::string& json) {
  wire::ClusterConfig cfg;
  google::protobuf::util::JsonParseOptions opts;
  opts.ignore_unknown_fields = false;
  auto st = google::protobuf::util::JsonStringToMessage(json, &cfg, opts);
  if (!st.ok()) return Status::InvalidArgument("config JSON: " + st.ToString());
  ApplyConfigDefaults(&cfg);
  DKV_RETURN_IF_ERROR(ValidateClusterConfig(cfg));
  return cfg;
}

StatusOr<wire::ClusterConfig> LoadClusterConfig(const std::string& path) {
  auto text = ReadFile(path);
  if (!text.ok()) return text.status();
  return ParseClusterConfig(*text);
}

std::string EndpointAddress(const wire::Endpoint& e) {
  return e.host() + ":" + std::to_string(e.port());
}

const wire::Endpoint* FindStorageNode(const wire::ClusterConfig& cfg, const std::string& id) {
  for (const auto& e : cfg.storage_nodes())
    if (e.id() == id) return &e;
  return nullptr;
}

std::string NodeDataDir(const wire::ClusterConfig& cfg, const std::string& id) {
  std::string base = ExpandVars(cfg.storage().data_dir(), {{"ID", id}});
  return JoinPath(base, id);
}

}  // namespace dkv
