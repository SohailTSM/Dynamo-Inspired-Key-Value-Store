#include "rpc/peer_client_pool.h"

namespace dkv {

PeerClientPool::PeerClientPool(std::map<std::string, std::string> addresses, ThreadPool& pool,
                               FaultInjector& faults)
    : addresses_(std::move(addresses)), pool_(pool), faults_(faults) {}

wire::ReplicaService::Stub* PeerClientPool::get(const std::string& peer) {
  std::lock_guard<std::mutex> l(mu_);
  auto it = stubs_.find(peer);
  if (it != stubs_.end()) return it->second.get();
  auto addr = addresses_.find(peer);
  if (addr == addresses_.end()) return nullptr;
  auto stub = wire::ReplicaService::NewStub(MakeChannel(addr->second));
  auto* raw = stub.get();
  stubs_[peer] = std::move(stub);
  return raw;
}

}  // namespace dkv
