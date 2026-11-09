// gRPC helpers shared by every process: channel creation, server start, status mapping (LLD §8).
#pragma once

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "common.pb.h"
#include "common/status.h"

namespace dkv {

// Insecure channel with short reconnect backoff so a restarted peer is reachable again quickly.
std::shared_ptr<grpc::Channel> MakeChannel(const std::string& address);

// Starts a server on 0.0.0.0:<port> with the given services. Returns nullptr on failure.
std::unique_ptr<grpc::Server> StartServer(uint32_t port, const std::vector<grpc::Service*>& services,
                                          int max_threads, std::string* error);

void SetDeadlineMs(grpc::ClientContext* ctx, int64_t timeout_ms);

wire::Code ToPbCode(StatusCode c);
void SetResult(wire::Result* r, wire::Code code, const std::string& msg = "");
const char* PbCodeName(wire::Code c);

}  // namespace dkv
