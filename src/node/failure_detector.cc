#include "node/failure_detector.h"

#include "common/logging.h"

namespace dkv {

const char* PeerHealthName(PeerHealth h) {
  switch (h) {
    case PeerHealth::kUp: return "UP";
    case PeerHealth::kSuspect: return "SUSPECT";
    case PeerHealth::kDown: return "DOWN";
  }
  return "?";
}

FailureDetector::FailureDetector(std::string self, std::vector<std::string> peers, Options opts,
                                 PeerClientPool& pool, Clock& clock, EventLog* events)
    : self_(std::move(self)), peers_(std::move(peers)), opts_(opts), pool_(pool), clock_(clock),
      events_(events) {
  int64_t now = clock_.now_mono_ms();
  for (const auto& p : peers_) state_[p].last_seen_mono_ms = now;  // assumed up at start
}

FailureDetector::~FailureDetector() { stop(); }

void FailureDetector::start() {
  sender_ = std::thread([this] { sender_loop(); });
  ticker_ = std::thread([this] { ticker_loop(); });
}

void FailureDetector::stop() {
  {
    std::lock_guard<std::mutex> l(stop_mu_);
    stopping_ = true;
  }
  stop_cv_.notify_all();
  if (sender_.joinable()) sender_.join();
  if (ticker_.joinable()) ticker_.join();
}

wire::HeartbeatMsg FailureDetector::make_heartbeat() {
  wire::HeartbeatMsg m;
  m.set_from(self_);
  m.set_incarnation(incarnation_);
  m.set_seq(++seq_);
  m.set_sent_wall_ms(clock_.now_wall_ms());
  for (const auto& s : suspects()) m.add_suspects(s);
  return m;
}

void FailureDetector::sender_loop() {
  for (;;) {
    wire::HeartbeatMsg msg = make_heartbeat();
    const uint64_t my_inc = incarnation_;
    for (const auto& p : peers_) {
      pool_.async<wire::HeartbeatMsg, wire::HeartbeatAck>(
          p, &wire::ReplicaService::Stub::Heartbeat, msg, opts_.rpc_timeout_ms,
          [this, p, my_inc](const grpc::Status& st, const wire::HeartbeatAck& ack) {
            if (st.ok() && ack.seen_sender_incarnation() == my_inc) {
              std::lock_guard<std::mutex> l(mu_);
              acked_.insert(p);
            }
          });
    }
    std::unique_lock<std::mutex> l(stop_mu_);
    if (stop_cv_.wait_for(l, std::chrono::milliseconds(opts_.interval_ms), [this] { return stopping_; }))
      return;
  }
}

void FailureDetector::ticker_loop() {
  for (;;) {
    tick();
    std::unique_lock<std::mutex> l(stop_mu_);
    if (stop_cv_.wait_for(l, std::chrono::milliseconds(opts_.tick_ms), [this] { return stopping_; }))
      return;
  }
}

void FailureDetector::tick() {
  int64_t now = clock_.now_mono_ms();
  std::vector<std::tuple<std::string, PeerHealth, PeerHealth, int64_t>> changes;
  {
    std::lock_guard<std::mutex> l(mu_);
    for (auto& kv : state_) {
      auto& s = kv.second;
      int64_t missed = (now - s.last_seen_mono_ms) / int64_t(opts_.interval_ms);
      PeerHealth h = PeerHealth::kUp;
      if (missed >= int64_t(opts_.down_after))
        h = PeerHealth::kDown;
      else if (missed >= int64_t(opts_.suspect_after))
        h = PeerHealth::kSuspect;
      if (h != s.health) {
        changes.emplace_back(kv.first, s.health, h, missed);
        s.health = h;
      }
    }
  }
  for (auto& c : changes) {
    DKV_LOG(INFO) << "fd: peer " << std::get<0>(c) << " " << PeerHealthName(std::get<1>(c)) << " -> "
                  << PeerHealthName(std::get<2>(c));
    if (events_)
      events_->emit(Ev("fd.transition")
                        .str("peer", std::get<0>(c))
                        .str("from", PeerHealthName(std::get<1>(c)))
                        .str("to", PeerHealthName(std::get<2>(c)))
                        .i64("missed", std::get<3>(c)));
  }
}

void FailureDetector::on_heartbeat(const wire::HeartbeatMsg& m) {
  PeerHealth before;
  bool restarted = false;
  {
    std::lock_guard<std::mutex> l(mu_);
    auto it = state_.find(m.from());
    if (it == state_.end()) return;
    auto& s = it->second;
    before = s.health;
    restarted = s.incarnation != 0 && m.incarnation() > s.incarnation;
    s.last_seen_mono_ms = clock_.now_mono_ms();
    s.incarnation = std::max(s.incarnation, m.incarnation());
    s.health = PeerHealth::kUp;
  }
  if (before != PeerHealth::kUp || restarted) {
    DKV_LOG(INFO) << "fd: peer " << m.from() << " " << PeerHealthName(before) << " -> UP"
                  << (restarted ? " (restarted, incarnation " + std::to_string(m.incarnation()) + ")" : "");
    if (events_)
      events_->emit(Ev("fd.transition")
                        .str("peer", m.from())
                        .str("from", PeerHealthName(before))
                        .str("to", "UP")
                        .u64("incarnation", m.incarnation())
                        .boolean("restarted", restarted));
  }
}

uint64_t FailureDetector::seen_incarnation(const std::string& peer) const {
  std::lock_guard<std::mutex> l(mu_);
  auto it = state_.find(peer);
  return it == state_.end() ? 0 : it->second.incarnation;
}

bool FailureDetector::is_down(const std::string& peer) const { return health(peer) == PeerHealth::kDown; }

PeerHealth FailureDetector::health(const std::string& peer) const {
  std::lock_guard<std::mutex> l(mu_);
  auto it = state_.find(peer);
  return it == state_.end() ? PeerHealth::kUp : it->second.health;
}

std::vector<std::string> FailureDetector::suspects() const {
  std::lock_guard<std::mutex> l(mu_);
  std::vector<std::string> out;
  for (const auto& kv : state_)
    if (kv.second.health != PeerHealth::kUp) out.push_back(kv.first);
  return out;
}

bool FailureDetector::peer_acked_incarnation(const std::string& peer) const {
  std::lock_guard<std::mutex> l(mu_);
  return acked_.count(peer) > 0;
}

}  // namespace dkv
