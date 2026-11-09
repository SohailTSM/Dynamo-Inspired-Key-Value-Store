// One channel + ReplicaService stub per peer; async calls run sync stubs on a ThreadPool with
// deadlines (LLD §5.5, §9). Every outgoing call passes the FaultInjector first.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "common/thread_pool.h"
#include "node.grpc.pb.h"
#include "rpc/fault_injector.h"
#include "rpc/rpc_util.h"

namespace dkv {

class PeerClientPool {
 public:
  // addresses: peer id -> "host:port"
  PeerClientPool(std::map<std::string, std::string> addresses, ThreadPool& pool, FaultInjector& faults);

  template <typename Req, typename Resp>
  using Method = grpc::Status (wire::ReplicaService::Stub::*)(grpc::ClientContext*, const Req&, Resp*);

  // Synchronous call (used by anti-entropy and the failure detector's own threads).
  template <typename Req, typename Resp>
  grpc::Status call(const std::string& peer, Method<Req, Resp> m, const Req& req, Resp* resp,
                    int64_t timeout_ms) {
    if (!faults_.allow_outgoing(peer))
      return grpc::Status(grpc::StatusCode::UNAVAILABLE, "fault injected: drop outgoing to " + peer);
    uint32_t delay = faults_.delay_ms(peer);
    if (delay) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    wire::ReplicaService::Stub* stub = get(peer);
    if (!stub) return grpc::Status(grpc::StatusCode::NOT_FOUND, "unknown peer " + peer);
    grpc::ClientContext ctx;
    SetDeadlineMs(&ctx, timeout_ms);
    return (stub->*m)(&ctx, req, resp);
  }

  // Asynchronous call on the fan-out pool; cb(status, response) runs on a pool thread.
  template <typename Req, typename Resp>
  void async(const std::string& peer, Method<Req, Resp> m, Req req, int64_t timeout_ms,
             std::function<void(const grpc::Status&, const Resp&)> cb) {
    bool queued = pool_.submit([this, peer, m, req = std::move(req), timeout_ms, cb]() {
      Resp resp;
      grpc::Status st = call(peer, m, req, &resp, timeout_ms);
      if (cb) cb(st, resp);
    });
    if (!queued && cb) cb(grpc::Status(grpc::StatusCode::UNAVAILABLE, "shutting down"), Resp());
  }

  wire::ReplicaService::Stub* get(const std::string& peer);

 private:
  std::map<std::string, std::string> addresses_;
  ThreadPool& pool_;
  FaultInjector& faults_;
  std::mutex mu_;
  std::map<std::string, std::unique_ptr<wire::ReplicaService::Stub>> stubs_;
};

}  // namespace dkv
