#include "client/kv_client.h"

#include <chrono>

#include "common/fs.h"
#include "common/hash.h"
#include "rpc/rpc_util.h"
#include "version/vector_clock.h"

namespace dkv {

namespace {
int64_t NowUs() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}
wire::Code TransportCode(const grpc::Status& st) {
  return st.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED ? wire::TIMEOUT : wire::UNAVAILABLE;
}
}  // namespace

KvClient::KvClient(std::vector<std::string> router_addresses, int64_t timeout_ms) : timeout_ms_(timeout_ms) {
  for (const auto& a : router_addresses) stubs_.push_back(wire::KvService::NewStub(MakeChannel(a)));
}

PutResult KvClient::put(const std::string& key, const std::string& value, const std::string& request_id) {
  PutResult out;
  wire::PutRequest req;
  req.set_key(key);
  req.set_value(value);
  req.set_request_id(request_id.empty() ? RandomId() : request_id);
  const int64_t t0 = NowUs();
  size_t start = rr_++;
  for (size_t attempt = 0; attempt < std::min<size_t>(2, stubs_.size()); ++attempt) {
    auto& stub = stubs_[(start + attempt) % stubs_.size()];
    grpc::ClientContext ctx;
    SetDeadlineMs(&ctx, timeout_ms_);
    wire::PutResponse resp;
    grpc::Status st = stub->Put(&ctx, req, &resp);
    if (!st.ok()) {
      out.code = TransportCode(st);
      out.message = "router: " + st.error_message();
      continue;
    }
    out.code = resp.result().code();
    out.message = resp.result().message();
    out.version_digest = ToHex(reinterpret_cast<const uint8_t*>(resp.version().digest().data()),
                               resp.version().digest().size());
    out.clock = VectorClock::FromProto(resp.version().clock()).ToString();
    out.coordinator = resp.coordinator();
    out.acks = resp.acks();
    break;
  }
  out.latency_us = NowUs() - t0;
  return out;
}

GetResult KvClient::get(const std::string& key) {
  GetResult out;
  wire::GetRequest req;
  req.set_key(key);
  const int64_t t0 = NowUs();
  size_t start = rr_++;
  for (size_t attempt = 0; attempt < std::min<size_t>(2, stubs_.size()); ++attempt) {
    auto& stub = stubs_[(start + attempt) % stubs_.size()];
    grpc::ClientContext ctx;
    SetDeadlineMs(&ctx, timeout_ms_);
    wire::GetResponse resp;
    grpc::Status st = stub->Get(&ctx, req, &resp);
    if (!st.ok()) {
      out.code = TransportCode(st);
      out.message = "router: " + st.error_message();
      continue;
    }
    out.code = resp.result().code();
    out.message = resp.result().message();
    out.coordinator = resp.coordinator();
    out.siblings = uint32_t(resp.versions_size());
    if (resp.versions_size() > 0) {
      const auto& v = resp.versions(0);
      out.value = v.value();
      out.version_digest = ToHex(reinterpret_cast<const uint8_t*>(v.digest().data()), v.digest().size());
      out.clock = VectorClock::FromProto(v.clock()).ToString();
    }
    break;
  }
  out.latency_us = NowUs() - t0;
  return out;
}

}  // namespace dkv
