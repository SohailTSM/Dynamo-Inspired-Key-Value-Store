// gRPC service implementations of a storage node (LLD §5.5).
#pragma once

#include <string>

#include "merkle/merkle_tree.h"
#include "node.grpc.pb.h"
#include "node/coordinator.h"
#include "node/failure_detector.h"
#include "rpc/fault_injector.h"
#include "storage/storage_engine.h"

namespace dkv {

class CoordinatorServiceImpl final : public wire::CoordinatorService::Service {
 public:
  explicit CoordinatorServiceImpl(Coordinator& c) : coord_(c) {}
  grpc::Status CoordPut(grpc::ServerContext*, const wire::PutRequest* req, wire::PutResponse* resp) override;
  grpc::Status CoordGet(grpc::ServerContext*, const wire::GetRequest* req, wire::GetResponse* resp) override;

 private:
  Coordinator& coord_;
};

class ReplicaServiceImpl final : public wire::ReplicaService::Service {
 public:
  ReplicaServiceImpl(std::string self, uint64_t incarnation, StorageEngine& engine, MerkleForest& forest,
                     FailureDetector& fd, FaultInjector& faults)
      : self_(std::move(self)), incarnation_(incarnation), engine_(engine), forest_(forest), fd_(fd),
        faults_(faults) {}

  grpc::Status Replicate(grpc::ServerContext*, const wire::ReplicateRequest*, wire::ReplicateResponse*) override;
  grpc::Status ReadLocal(grpc::ServerContext*, const wire::ReadLocalRequest*, wire::ReadLocalResponse*) override;
  grpc::Status Heartbeat(grpc::ServerContext*, const wire::HeartbeatMsg*, wire::HeartbeatAck*) override;
  grpc::Status GetMerkleNodes(grpc::ServerContext*, const wire::MerkleRequest*, wire::MerkleResponse*) override;
  grpc::Status GetBucketDigests(grpc::ServerContext*, const wire::BucketRequest*, wire::BucketResponse*) override;
  grpc::Status FetchVersions(grpc::ServerContext*, const wire::FetchRequest*, wire::FetchResponse*) override;
  grpc::Status SetFaults(grpc::ServerContext*, const wire::FaultConfig*, wire::Result*) override;

 private:
  grpc::Status drop_if_faulted(const std::string& from) const;
  const std::string self_;
  const uint64_t incarnation_;
  StorageEngine& engine_;
  MerkleForest& forest_;
  FailureDetector& fd_;
  FaultInjector& faults_;
};

// Fills `out` with the current versions of `key` (values included unless metadata_only).
Status ReadCurrentVersions(const StorageEngine& engine, const std::string& key, bool metadata_only,
                           google::protobuf::RepeatedPtrField<wire::Version>* out);

}  // namespace dkv
