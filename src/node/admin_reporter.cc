#include "node/admin_reporter.h"

#include "common/logging.h"
#include "rpc/rpc_util.h"

namespace dkv {

AdminReporter::AdminReporter(std::string admin_address, std::string self, uint64_t incarnation,
                             int64_t process_start_wall_ms, uint32_t interval_ms, FailureDetector& fd,
                             Clock& clock)
    : self_(std::move(self)), incarnation_(incarnation), start_wall_ms_(process_start_wall_ms),
      interval_ms_(interval_ms), fd_(fd), clock_(clock),
      stub_(wire::AdminService::NewStub(MakeChannel(admin_address))) {}

AdminReporter::~AdminReporter() { stop(); }

void AdminReporter::start() { thread_ = std::thread([this] { loop(); }); }

void AdminReporter::stop() {
  {
    std::lock_guard<std::mutex> l(mu_);
    stopping_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void AdminReporter::report_repair(const wire::RepairReport& report) {
  {
    std::lock_guard<std::mutex> l(mu_);
    pending_.push_back(report);
  }
  cv_.notify_all();
}

bool AdminReporter::registered() const {
  std::lock_guard<std::mutex> l(mu_);
  return registered_;
}

bool AdminReporter::try_register() {
  grpc::ClientContext ctx;
  SetDeadlineMs(&ctx, 500);
  wire::RegisterRequest req;
  req.set_node(self_);
  req.set_incarnation(incarnation_);
  req.set_process_start_wall_ms(start_wall_ms_);
  wire::Result res;
  grpc::Status st = stub_->Register(&ctx, req, &res);
  return st.ok() && res.code() == wire::OK;
}

void AdminReporter::loop() {
  for (;;) {
    bool reg;
    {
      std::lock_guard<std::mutex> l(mu_);
      reg = registered_;
    }
    if (!reg && try_register()) {
      std::lock_guard<std::mutex> l(mu_);
      registered_ = true;
      DKV_LOG(INFO) << "registered with auto-admin (incarnation " << incarnation_ << ")";
    }
    // Heartbeat (carries suspects -> peer confirmation at the admin).
    {
      grpc::ClientContext ctx;
      SetDeadlineMs(&ctx, 500);
      wire::HeartbeatMsg hb = fd_.make_heartbeat();
      wire::HeartbeatAck ack;
      stub_->Heartbeat(&ctx, hb, &ack);
    }
    // Flush queued repair reports (keep on failure, retry next round).
    for (;;) {
      wire::RepairReport r;
      {
        std::lock_guard<std::mutex> l(mu_);
        if (pending_.empty() || !registered_) break;
        r = pending_.front();
      }
      grpc::ClientContext ctx;
      SetDeadlineMs(&ctx, 500);
      wire::Result res;
      if (!stub_->ReportRepair(&ctx, r, &res).ok()) break;
      std::lock_guard<std::mutex> l(mu_);
      pending_.pop_front();
    }
    std::unique_lock<std::mutex> l(mu_);
    if (cv_.wait_for(l, std::chrono::milliseconds(interval_ms_), [this] { return stopping_; })) return;
  }
}

}  // namespace dkv
