#include "node/services.h"

#include "common/logging.h"

namespace dkv {

grpc::Status CoordinatorServiceImpl::CoordPut(grpc::ServerContext*, const wire::PutRequest* req,
                                              wire::PutResponse* resp) {
  coord_.put(*req, resp);
  return grpc::Status::OK;
}

grpc::Status CoordinatorServiceImpl::CoordGet(grpc::ServerContext*, const wire::GetRequest* req,
                                              wire::GetResponse* resp) {
  coord_.get(*req, resp);
  return grpc::Status::OK;
}

Status ReadCurrentVersions(const StorageEngine& engine, const std::string& key, bool metadata_only,
                           google::protobuf::RepeatedPtrField<wire::Version>* out) {
  for (const auto& m : engine.current_versions(key)) {
    if (metadata_only) {
      MetaToProto(m, out->Add());
    } else {
      auto full = engine.read_version(key, m);
      if (!full.ok()) return full.status();
      *out->Add() = *full;
    }
  }
  return Status::OK();
}

grpc::Status ReplicaServiceImpl::drop_if_faulted(const std::string& from) const {
  if (!faults_.allow_incoming(from))
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "fault injected: drop incoming from " + from);
  return grpc::Status::OK;
}

grpc::Status ReplicaServiceImpl::Replicate(grpc::ServerContext*, const wire::ReplicateRequest* req,
                                           wire::ReplicateResponse* resp) {
  auto f = drop_if_faulted(req->from());
  if (!f.ok()) return f;
  const auto& v = req->version();
  // End-to-end integrity: the digest stamped by the coordinator must match the bytes we received.
  Digest160 expect = ComputeVersionDigest(VectorClock::FromProto(v.clock()), v.timestamp_ms(),
                                          v.coordinator_id(), v.value());
  if (DigestToBytes(expect) != v.digest()) {
    SetResult(resp->mutable_result(), wire::INVALID_ARGUMENT, "version digest mismatch");
    return grpc::Status::OK;
  }
  auto st = engine_.put_version(req->key(), v);
  if (!st.ok()) {
    SetResult(resp->mutable_result(), wire::INTERNAL, st.status().ToString());
    return grpc::Status::OK;
  }
  resp->set_stored(*st);
  SetResult(resp->mutable_result(), wire::OK);
  return grpc::Status::OK;
}

grpc::Status ReplicaServiceImpl::ReadLocal(grpc::ServerContext*, const wire::ReadLocalRequest* req,
                                           wire::ReadLocalResponse* resp) {
  auto f = drop_if_faulted(req->from());
  if (!f.ok()) return f;
  Status st = ReadCurrentVersions(engine_, req->key(), req->metadata_only(), resp->mutable_versions());
  SetResult(resp->mutable_result(), st.ok() ? wire::OK : wire::INTERNAL, st.ok() ? "" : st.ToString());
  return grpc::Status::OK;
}

grpc::Status ReplicaServiceImpl::Heartbeat(grpc::ServerContext*, const wire::HeartbeatMsg* m,
                                           wire::HeartbeatAck* ack) {
  auto f = drop_if_faulted(m->from());
  if (!f.ok()) return f;
  fd_.on_heartbeat(*m);
  ack->set_from(self_);
  ack->set_incarnation(incarnation_);
  ack->set_seen_sender_incarnation(fd_.seen_incarnation(m->from()));
  return grpc::Status::OK;
}

grpc::Status ReplicaServiceImpl::GetMerkleNodes(grpc::ServerContext*, const wire::MerkleRequest* req,
                                                wire::MerkleResponse* resp) {
  auto f = drop_if_faulted(req->from());
  if (!f.ok()) return f;
  for (const auto& ref : req->nodes()) {
    auto h = forest_.node_hash(ref.range_id(), ref.node_index());
    resp->add_hashes(h ? DigestToBytes(*h) : std::string());  // empty = not a replica of that range
  }
  return grpc::Status::OK;
}

grpc::Status ReplicaServiceImpl::GetBucketDigests(grpc::ServerContext*, const wire::BucketRequest* req,
                                                  wire::BucketResponse* resp) {
  auto f = drop_if_faulted(req->from());
  if (!f.ok()) return f;
  for (uint32_t leaf : req->leaves()) {
    auto contents = forest_.leaf_contents(req->range_id(), leaf);
    if (!contents) continue;
    for (const auto& kd : *contents) {
      auto* k = resp->add_keys();
      k->set_key(kd.first);
      for (const auto& d : kd.second) k->add_version_digests(DigestToBytes(d));
    }
  }
  return grpc::Status::OK;
}

grpc::Status ReplicaServiceImpl::FetchVersions(grpc::ServerContext*, const wire::FetchRequest* req,
                                               wire::FetchResponse* resp) {
  auto f = drop_if_faulted(req->from());
  if (!f.ok()) return f;
  for (const auto& key : req->keys()) {
    auto* item = resp->add_items();
    item->set_key(key);
    Status st = ReadCurrentVersions(engine_, key, false, item->mutable_versions());
    if (!st.ok()) DKV_LOG(WARN) << "FetchVersions " << key << ": " << st.ToString();
  }
  return grpc::Status::OK;
}

grpc::Status ReplicaServiceImpl::SetFaults(grpc::ServerContext*, const wire::FaultConfig* cfg, wire::Result* res) {
  faults_.set(*cfg);
  DKV_LOG(WARN) << "fault rules set: " << cfg->rules_size() << " rule(s)";
  SetResult(res, wire::OK);
  return grpc::Status::OK;
}

}  // namespace dkv
