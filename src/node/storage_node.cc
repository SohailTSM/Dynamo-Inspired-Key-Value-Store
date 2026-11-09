#include "node/storage_node.h"

#include <unistd.h>

#include <cstdlib>

#include "common/config_loader.h"
#include "common/fs.h"
#include "common/logging.h"

namespace dkv {

StorageNode::StorageNode(wire::ClusterConfig cfg, std::string id) : cfg_(std::move(cfg)), id_(std::move(id)) {}

StorageNode::~StorageNode() { stop(); }

Status StorageNode::bump_incarnation() {
  std::string meta = JoinPath(data_dir_, "node.meta");
  uint64_t inc = 0;
  if (FileExists(meta)) {
    auto txt = ReadFile(meta);
    if (!txt.ok()) return txt.status();
    auto pos = txt->find("\"incarnation\":");
    if (pos != std::string::npos) inc = std::strtoull(txt->c_str() + pos + 14, nullptr, 10);
  }
  incarnation_ = inc + 1;
  return WriteFileAtomic(meta, "{\"id\":\"" + id_ + "\",\"incarnation\":" + std::to_string(incarnation_) + "}\n");
}

Status StorageNode::start() {
  start_wall_ms_ = clock_.now_wall_ms();
  const int64_t start_mono = clock_.now_mono_ms();
  const wire::Endpoint* self = FindStorageNode(cfg_, id_);
  if (!self) return Status::InvalidArgument("node id " + id_ + " not in config");
  data_dir_ = NodeDataDir(cfg_, id_);
  DKV_RETURN_IF_ERROR(MakeDirs(data_dir_));
  DKV_RETURN_IF_ERROR(bump_incarnation());
  DKV_RETURN_IF_ERROR(events_.open(JoinPath(data_dir_, "events.jsonl"), "node-" + id_, &clock_));
  events_.emit(Ev("proc.start")
                   .u64("incarnation", incarnation_)
                   .i64("start_wall_ms", start_wall_ms_)
                   .i64("start_mono_ms", start_mono)
                   .i64("pid", ::getpid()));
  DKV_LOG(INFO) << "node " << id_ << " starting, incarnation " << incarnation_ << ", data " << data_dir_;

  // Recovery: replay the version log into the index.
  engine_ = std::make_unique<LogStorageEngine>(data_dir_, cfg_.storage().segment_bytes());
  engine_->set_change_listener(
      [this](const std::string& key, const std::vector<VersionMeta>& cur) { forest_.on_current_changed(key, cur); });
  int64_t rec_t0 = clock_.now_mono_ms();
  DKV_RETURN_IF_ERROR(engine_->open());
  const auto& rs = engine_->recovery_stats();
  int64_t rec_ms = clock_.now_mono_ms() - rec_t0;
  DKV_LOG(INFO) << "recovery: " << rs.records << " records, " << engine_->stats().keys << " keys, "
                << rs.truncated_bytes << " torn bytes truncated, " << rec_ms << " ms";
  events_.emit(Ev("recovery.done")
                   .u64("records", rs.records)
                   .u64("segments", rs.segments)
                   .u64("keys", engine_->stats().keys)
                   .u64("truncated_bytes", rs.truncated_bytes)
                   .i64("ms", rec_ms));

  // Ring (Metadata, else config) and Merkle forest.
  if (!cfg_.metadata().host().empty())
    meta_client_ = std::make_unique<GrpcMetadataClient>(EndpointAddress(cfg_.metadata()));
  ring_ = std::make_unique<RingCache>(meta_client_.get(), cfg_);
  ring_->start();
  forest_.rebuild(ring_->current(), id_, *engine_);

  // Networking.
  std::map<std::string, std::string> addrs;
  std::vector<std::string> peer_ids;
  for (const auto& e : cfg_.storage_nodes()) {
    if (e.id() == id_) continue;
    addrs[e.id()] = EndpointAddress(e);
    peer_ids.push_back(e.id());
  }
  pool_ = std::make_unique<ThreadPool>(64);
  peers_ = std::make_unique<PeerClientPool>(addrs, *pool_, faults_);
  FailureDetector::Options fo;
  fo.interval_ms = cfg_.timing().heartbeat_interval_ms();
  fo.suspect_after = cfg_.timing().suspect_after_missed();
  fo.down_after = cfg_.timing().down_after_missed();
  fo.tick_ms = cfg_.timing().detector_tick_ms();
  fd_ = std::make_unique<FailureDetector>(id_, peer_ids, fo, *peers_, clock_, &events_);
  fd_->set_incarnation(incarnation_);
  coordinator_ = std::make_unique<Coordinator>(id_, cfg_, *ring_, *fd_, *engine_, *peers_, *pool_, clock_, &events_);
  coord_svc_ = std::make_unique<CoordinatorServiceImpl>(*coordinator_);
  replica_svc_ = std::make_unique<ReplicaServiceImpl>(id_, incarnation_, *engine_, forest_, *fd_, faults_);
  std::string err;
  server_ = StartServer(self->port(), {coord_svc_.get(), replica_svc_.get()}, 128, &err);
  if (!server_) return Status::Unavailable(err);
  fd_->start();

  if (!cfg_.admin().host().empty()) {
    admin_ = std::make_unique<AdminReporter>(EndpointAddress(cfg_.admin()), id_, incarnation_, start_wall_ms_,
                                             cfg_.timing().heartbeat_interval_ms(), *fd_, clock_);
    admin_->start();
  }

  AntiEntropy::Options ao;
  ao.interval_s = cfg_.timing().anti_entropy_interval_s();
  ao.coord_timeout_ms = cfg_.timing().coord_timeout_ms();
  anti_entropy_ = std::make_unique<AntiEntropy>(id_, incarnation_, start_wall_ms_, ao, forest_, *engine_,
                                                *peers_, *fd_, admin_.get(), clock_, &events_);
  anti_entropy_->start();
  DKV_LOG(INFO) << "node " << id_ << " serving on port " << self->port() << " (" << forest_.ranges().size()
                << " ranges replicated)";
  return Status::OK();
}

void StorageNode::wait() {
  if (server_) server_->Wait();
}

void StorageNode::stop() {
  if (stopped_.exchange(true)) return;
  DKV_LOG(INFO) << "node " << id_ << " stopping";
  const int64_t t0 = clock_.now_mono_ms();
  // 1. Stop everything that originates traffic.
  if (anti_entropy_) anti_entropy_->stop();
  if (admin_) admin_->stop();
  if (fd_) fd_->stop();
  if (ring_) ring_->stop();
  // 2. Stop serving: in-flight handlers get a short drain window, then are cancelled.
  if (server_) server_->Shutdown(std::chrono::system_clock::now() + std::chrono::milliseconds(500));
  // 3. Finish queued fan-out calls (bounded by their own RPC deadlines), then flush the log.
  if (pool_) pool_->stop();
  if (engine_) engine_->close();
  DKV_LOG(INFO) << "node " << id_ << " stopped in " << (clock_.now_mono_ms() - t0) << " ms";
  events_.emit(Ev("proc.stop").u64("incarnation", incarnation_));
  events_.close();
}

}  // namespace dkv
