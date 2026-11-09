// Client library (LLD §5.9): talks to the routers, retries once on another router after a transport
// error (same request_id, so the coordinator can drop a duplicate).
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "common.pb.h"
#include "kv.grpc.pb.h"

namespace dkv {

struct PutResult {
  wire::Code code = wire::UNAVAILABLE;
  std::string message;
  std::string version_digest;  // hex
  std::string clock;           // "{A:3,B:1}"
  std::string coordinator;
  uint32_t acks = 0;
  int64_t latency_us = 0;
};

struct GetResult {
  wire::Code code = wire::UNAVAILABLE;
  std::string message;
  std::string value;
  std::string version_digest;  // hex
  std::string clock;
  std::string coordinator;
  uint32_t siblings = 0;
  int64_t latency_us = 0;
};

class KvClient {
 public:
  explicit KvClient(std::vector<std::string> router_addresses, int64_t timeout_ms = 2000);
  PutResult put(const std::string& key, const std::string& value, const std::string& request_id = "");
  GetResult get(const std::string& key);

 private:
  std::vector<std::unique_ptr<wire::KvService::Stub>> stubs_;
  int64_t timeout_ms_;
  std::atomic<size_t> rr_{0};
};

}  // namespace dkv
