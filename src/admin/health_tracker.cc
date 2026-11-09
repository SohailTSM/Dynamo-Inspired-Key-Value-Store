#include "admin/health_tracker.h"

#include "common/fs.h"
#include "common/logging.h"

namespace dkv {

const char* HealthName(wire::NodeHealth h) {
  switch (h) {
    case wire::HEALTH_UNKNOWN: return "UNKNOWN";
    case wire::UP: return "UP";
    case wire::SUSPECT: return "SUSPECT";
    case wire::UNREACHABLE_FROM_ADMIN: return "UNREACHABLE_FROM_ADMIN";
    case wire::DOWN: return "DOWN";
    case wire::RECOVERING: return "RECOVERING";
    default: return "?";
  }
}

HealthTracker::HealthTracker(std::map<std::string, std::string> nodes, Options opts, Clock& clock,
                             EventLog* events, const std::string& out_dir)
    : opts_(opts), clock_(clock), events_(events) {
  for (auto& kv : nodes) nodes_[kv.first].address = kv.second;
  if (!out_dir.empty() && MakeDirs(out_dir).ok()) {
    std::string hp = JoinPath(out_dir, "health_events.csv");
    bool new_h = !FileExists(hp);
    health_csv_ = std::fopen(hp.c_str(), "a");
    if (health_csv_ && new_h)
      std::fprintf(health_csv_, "t_wall_ms,node,from,to,incarnation,last_hb_wall_ms,reason\n");
    std::string rp = JoinPath(out_dir, "repairs.csv");
    bool new_r = !FileExists(rp);
    repairs_csv_ = std::fopen(rp.c_str(), "a");
    if (repairs_csv_ && new_r)
      std::fprintf(repairs_csv_,
                   "admin_wall_ms,node,incarnation,process_start_wall_ms,repair_start_wall_ms,done_wall_ms,"
                   "repair_ms_from_start,repair_ms_pass,keys_fetched,bytes_fetched,ranges\n");
  }
}

HealthTracker::~HealthTracker() {
  stop();
  if (health_csv_) std::fclose(health_csv_);
  if (repairs_csv_) std::fclose(repairs_csv_);
}

void HealthTracker::start() {
  ticker_ = std::thread([this] {
    for (;;) {
      tick();
      std::unique_lock<std::mutex> l(stop_mu_);
      if (stop_cv_.wait_for(l, std::chrono::milliseconds(opts_.tick_ms), [this] { return stopping_; })) return;
    }
  });
}

void HealthTracker::stop() {
  {
    std::lock_guard<std::mutex> l(stop_mu_);
    stopping_ = true;
  }
  stop_cv_.notify_all();
  changed_.notify_all();
  if (ticker_.joinable()) ticker_.join();
}

void HealthTracker::transition_locked(const std::string& node, NodeRecord& rec, wire::NodeHealth to,
                                      const std::string& why) {
  if (rec.health == to) return;
  wire::NodeHealth from = rec.health;
  rec.health = to;
  ++version_;
  const int64_t now = clock_.now_wall_ms();
  DKV_LOG(INFO) << "health: " << node << " " << HealthName(from) << " -> " << HealthName(to) << " (" << why << ")";
  if (events_)
    events_->emit(Ev("admin.health")
                      .str("node", node)
                      .str("from", HealthName(from))
                      .str("to", HealthName(to))
                      .u64("incarnation", rec.incarnation)
                      .i64("last_hb_wall_ms", rec.last_hb_wall_ms)
                      .i64("since_last_hb_ms", rec.last_hb_wall_ms ? now - rec.last_hb_wall_ms : -1)
                      .str("reason", why));
  if (health_csv_) {
    std::fprintf(health_csv_, "%lld,%s,%s,%s,%llu,%lld,%s\n", (long long)now, node.c_str(), HealthName(from),
                 HealthName(to), (unsigned long long)rec.incarnation, (long long)rec.last_hb_wall_ms, why.c_str());
    std::fflush(health_csv_);
  }
  changed_.notify_all();
}

void HealthTracker::on_heartbeat(const wire::HeartbeatMsg& m) {
  std::lock_guard<std::mutex> l(mu_);
  auto it = nodes_.find(m.from());
  if (it == nodes_.end()) return;
  auto& rec = it->second;
  rec.last_hb_mono_ms = clock_.now_mono_ms();
  rec.last_hb_wall_ms = clock_.now_wall_ms();
  rec.reports_suspect = std::set<std::string>(m.suspects().begin(), m.suspects().end());
  bool new_inc = m.incarnation() > rec.incarnation;
  if (new_inc) rec.incarnation = m.incarnation();
  switch (rec.health) {
    case wire::HEALTH_UNKNOWN:
      transition_locked(m.from(), rec, m.incarnation() > 1 ? wire::RECOVERING : wire::UP, "first heartbeat");
      break;
    case wire::DOWN:
      transition_locked(m.from(), rec, new_inc ? wire::RECOVERING : wire::UP,
                        new_inc ? "heartbeat from new incarnation" : "heartbeat (false DOWN)");
      break;
    case wire::SUSPECT:
    case wire::UNREACHABLE_FROM_ADMIN:
      transition_locked(m.from(), rec, new_inc ? wire::RECOVERING : wire::UP, "heartbeat");
      break;
    default:
      break;  // UP / RECOVERING stay
  }
}

void HealthTracker::on_register(const wire::RegisterRequest& r) {
  std::lock_guard<std::mutex> l(mu_);
  auto it = nodes_.find(r.node());
  if (it == nodes_.end()) return;
  auto& rec = it->second;
  if (r.incarnation() > rec.incarnation || rec.health == wire::HEALTH_UNKNOWN) {
    rec.incarnation = std::max(rec.incarnation, r.incarnation());
    rec.last_hb_mono_ms = clock_.now_mono_ms();
    rec.last_hb_wall_ms = clock_.now_wall_ms();
    transition_locked(r.node(), rec, r.incarnation() > 1 ? wire::RECOVERING : wire::UP,
                      "register incarnation " + std::to_string(r.incarnation()));
    if (events_)
      events_->emit(Ev("admin.recovery")
                        .str("node", r.node())
                        .u64("incarnation", r.incarnation())
                        .i64("process_start_wall_ms", r.process_start_wall_ms()));
  }
}

void HealthTracker::on_repair_report(const wire::RepairReport& r) {
  std::lock_guard<std::mutex> l(mu_);
  auto it = nodes_.find(r.node());
  if (it == nodes_.end()) return;
  auto& rec = it->second;
  if (r.incarnation() < rec.incarnation) return;  // stale report from an older incarnation
  repairs_[r.node()] = r;
  if (r.phase() == wire::RepairReport::STARTED && rec.health == wire::UP && r.incarnation() > 1) {
    transition_locked(r.node(), rec, wire::RECOVERING, "repair started");
  } else if (r.phase() == wire::RepairReport::DONE) {
    int64_t from_start = r.report_wall_ms() - r.process_start_wall_ms();
    int64_t pass = r.report_wall_ms() - r.repair_start_wall_ms();
    DKV_LOG(INFO) << "repair done: " << r.node() << " incarnation " << r.incarnation() << " in " << from_start
                  << " ms from process start (" << pass << " ms pass), " << r.keys_fetched() << " keys";
    if (events_)
      events_->emit(Ev("admin.repair_done")
                        .str("node", r.node())
                        .u64("incarnation", r.incarnation())
                        .i64("repair_ms_from_start", from_start)
                        .i64("repair_ms_pass", pass)
                        .u64("keys_fetched", r.keys_fetched())
                        .u64("bytes_fetched", r.bytes_fetched()));
    if (repairs_csv_) {
      std::fprintf(repairs_csv_, "%lld,%s,%llu,%lld,%lld,%lld,%lld,%lld,%llu,%llu,%llu\n",
                   (long long)clock_.now_wall_ms(), r.node().c_str(), (unsigned long long)r.incarnation(),
                   (long long)r.process_start_wall_ms(), (long long)r.repair_start_wall_ms(),
                   (long long)r.report_wall_ms(), (long long)from_start, (long long)pass,
                   (unsigned long long)r.keys_fetched(), (unsigned long long)r.bytes_fetched(),
                   (unsigned long long)r.ranges_total());
      std::fflush(repairs_csv_);
    }
    if (rec.health == wire::RECOVERING) transition_locked(r.node(), rec, wire::UP, "repair done");
  }
  ++version_;
  changed_.notify_all();
}

void HealthTracker::tick() {
  std::lock_guard<std::mutex> l(mu_);
  const int64_t now = clock_.now_mono_ms();
  for (auto& kv : nodes_) {
    auto& rec = kv.second;
    if (rec.health == wire::HEALTH_UNKNOWN || rec.health == wire::DOWN) continue;
    int64_t missed = (now - rec.last_hb_mono_ms) / int64_t(opts_.interval_ms);
    if (missed >= int64_t(opts_.down_after)) {
      // Peer confirmation (gray-failure rule): DOWN only if a majority of the other alive nodes
      // also suspect it.
      int alive_others = 0, confirm = 0;
      for (const auto& o : nodes_) {
        if (o.first == kv.first || !alive(o.second.health)) continue;
        if (o.second.health == wire::UNREACHABLE_FROM_ADMIN) continue;  // its reports are stale too
        ++alive_others;
        if (o.second.reports_suspect.count(kv.first)) ++confirm;
      }
      if (alive_others == 0 || confirm * 2 > alive_others)
        transition_locked(kv.first, rec, wire::DOWN,
                          "missed " + std::to_string(missed) + " heartbeats, confirmed by " + std::to_string(confirm) +
                              "/" + std::to_string(alive_others) + " peers");
      else
        transition_locked(kv.first, rec, wire::UNREACHABLE_FROM_ADMIN,
                          "missed " + std::to_string(missed) + " heartbeats but only " + std::to_string(confirm) +
                              "/" + std::to_string(alive_others) + " peers suspect it");
    } else if (missed >= int64_t(opts_.suspect_after) && (rec.health == wire::UP || rec.health == wire::RECOVERING)) {
      transition_locked(kv.first, rec, wire::SUSPECT, "missed " + std::to_string(missed) + " heartbeat(s)");
    }
  }
}

wire::HealthSnapshot HealthTracker::snapshot() const {
  std::lock_guard<std::mutex> l(mu_);
  wire::HealthSnapshot s;
  s.set_version(version_);
  for (const auto& kv : nodes_) {
    auto* n = s.add_nodes();
    n->set_node(kv.first);
    n->set_health(kv.second.health);
    n->set_incarnation(kv.second.incarnation);
    n->set_last_heartbeat_admin_ms(kv.second.last_hb_wall_ms);
    n->set_address(kv.second.address);
    for (const auto& o : nodes_)
      if (o.second.reports_suspect.count(kv.first)) n->add_suspected_by(o.first);
  }
  return s;
}

wire::ClusterStatus HealthTracker::status() const {
  wire::ClusterStatus st;
  *st.mutable_health() = snapshot();
  std::lock_guard<std::mutex> l(mu_);
  for (const auto& kv : repairs_) *st.add_last_repairs() = kv.second;
  return st;
}

wire::HealthSnapshot HealthTracker::wait_for_change(uint64_t after_version, int64_t timeout_ms) {
  {
    std::unique_lock<std::mutex> l(mu_);
    changed_.wait_for(l, std::chrono::milliseconds(timeout_ms), [&] { return version_ != after_version; });
  }
  return snapshot();
}

}  // namespace dkv
