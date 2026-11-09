#include "rpc/rpc_util.h"

namespace dkv {

std::shared_ptr<grpc::Channel> MakeChannel(const std::string& address) {
  grpc::ChannelArguments args;
  args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 100);
  args.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, 100);
  args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, 1000);
  args.SetInt(GRPC_ARG_MAX_RECEIVE_MESSAGE_LENGTH, 64 << 20);
  return grpc::CreateCustomChannel(address, grpc::InsecureChannelCredentials(), args);
}

std::unique_ptr<grpc::Server> StartServer(uint32_t port, const std::vector<grpc::Service*>& services,
                                          int max_threads, std::string* error) {
  grpc::ServerBuilder b;
  int selected = 0;
  b.AddListeningPort("0.0.0.0:" + std::to_string(port), grpc::InsecureServerCredentials(), &selected);
  for (auto* s : services) b.RegisterService(s);
  grpc::ResourceQuota quota("dkv");
  quota.SetMaxThreads(max_threads);
  b.SetResourceQuota(quota);
  b.SetMaxReceiveMessageSize(64 << 20);
  auto server = b.BuildAndStart();
  if (!server || selected == 0) {
    if (error) *error = "could not listen on port " + std::to_string(port);
    return nullptr;
  }
  return server;
}

void SetDeadlineMs(grpc::ClientContext* ctx, int64_t timeout_ms) {
  ctx->set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(timeout_ms));
}

wire::Code ToPbCode(StatusCode c) {
  switch (c) {
    case StatusCode::kOk: return wire::OK;
    case StatusCode::kNotFound: return wire::NOT_FOUND;
    case StatusCode::kQuorumNotReached: return wire::QUORUM_NOT_REACHED;
    case StatusCode::kInvalidArgument: return wire::INVALID_ARGUMENT;
    case StatusCode::kUnavailable: return wire::UNAVAILABLE;
    case StatusCode::kTimeout: return wire::TIMEOUT;
    case StatusCode::kNotAReplica: return wire::NOT_A_REPLICA;
    default: return wire::INTERNAL;
  }
}

void SetResult(wire::Result* r, wire::Code code, const std::string& msg) {
  r->set_code(code);
  r->set_message(msg);
}

const char* PbCodeName(wire::Code c) {
  switch (c) {
    case wire::OK: return "OK";
    case wire::NOT_FOUND: return "NOT_FOUND";
    case wire::QUORUM_NOT_REACHED: return "QUORUM_NOT_REACHED";
    case wire::INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case wire::UNAVAILABLE: return "UNAVAILABLE";
    case wire::TIMEOUT: return "TIMEOUT";
    case wire::NOT_A_REPLICA: return "NOT_A_REPLICA";
    case wire::INTERNAL: return "INTERNAL";
    default: return "UNKNOWN";
  }
}

}  // namespace dkv
