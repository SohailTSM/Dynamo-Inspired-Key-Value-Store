// dkv_cli --config <cluster.json> <command>
//   put <key> <value>        get <key>        status
//   faults <node> clear      faults <node> drop-out <peer>[,<peer>...]   (drop outgoing to peers)
//   faults <node> drop-in <peer>[,...]       faults <node> delay <peer> <ms>
#include <google/protobuf/util/json_util.h>

#include <cstdio>
#include <sstream>

#include "admin.grpc.pb.h"
#include "client/kv_client.h"
#include "common/config_loader.h"
#include "common/flags.h"
#include "node.grpc.pb.h"
#include "rpc/rpc_util.h"

using namespace dkv;

static std::vector<std::string> Split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, sep))
    if (!item.empty()) out.push_back(item);
  return out;
}

static int Usage() {
  std::fprintf(stderr,
               "usage: dkv_cli --config <cluster.json> put <k> <v> | get <k> | status |\n"
               "       faults <node> clear | faults <node> drop-out|drop-in <p1,p2> | faults <node> delay <peer> <ms>\n");
  return 2;
}

int main(int argc, char** argv) {
  Flags flags(argc, argv);
  const auto& args = flags.positional();
  if (!flags.has("config") || args.empty()) return Usage();
  auto cfg = LoadClusterConfig(flags.get("config"));
  if (!cfg.ok()) {
    std::fprintf(stderr, "%s\n", cfg.status().ToString().c_str());
    return 1;
  }
  std::vector<std::string> routers;
  for (const auto& r : cfg->routers()) routers.push_back(EndpointAddress(r));
  const std::string& cmd = args[0];

  if (cmd == "put" && args.size() == 3) {
    KvClient c(routers);
    auto r = c.put(args[1], args[2]);
    std::printf("%s acks=%u coordinator=%s clock=%s latency=%.2fms %s\n", PbCodeName(r.code), r.acks,
                r.coordinator.c_str(), r.clock.c_str(), r.latency_us / 1000.0, r.message.c_str());
    return r.code == wire::OK ? 0 : 1;
  }
  if (cmd == "get" && args.size() == 2) {
    KvClient c(routers);
    auto r = c.get(args[1]);
    if (r.code == wire::OK)
      std::printf("OK value=%s clock=%s coordinator=%s latency=%.2fms\n", r.value.c_str(), r.clock.c_str(),
                  r.coordinator.c_str(), r.latency_us / 1000.0);
    else
      std::printf("%s %s\n", PbCodeName(r.code), r.message.c_str());
    return r.code == wire::OK ? 0 : 1;
  }
  if (cmd == "status") {
    auto stub = wire::AdminService::NewStub(MakeChannel(EndpointAddress(cfg->admin())));
    grpc::ClientContext ctx;
    SetDeadlineMs(&ctx, 2000);
    wire::Empty e;
    wire::ClusterStatus st;
    auto s = stub->GetClusterStatus(&ctx, e, &st);
    if (!s.ok()) {
      std::fprintf(stderr, "admin unreachable: %s\n", s.error_message().c_str());
      return 1;
    }
    std::printf("%-6s %-24s %-12s %-20s %s\n", "NODE", "HEALTH", "INCARNATION", "ADDRESS", "SUSPECTED_BY");
    for (const auto& n : st.health().nodes()) {
      std::string by;
      for (const auto& x : n.suspected_by()) by += x + " ";
      std::printf("%-6s %-24s %-12llu %-20s %s\n", n.node().c_str(), wire::NodeHealth_Name(n.health()).c_str(),
                  (unsigned long long)n.incarnation(), n.address().c_str(), by.c_str());
    }
    for (const auto& r : st.last_repairs())
      std::printf("repair %s inc=%llu phase=%s repair_ms_from_start=%lld keys=%llu\n", r.node().c_str(),
                  (unsigned long long)r.incarnation(), wire::RepairReport::Phase_Name(r.phase()).c_str(),
                  (long long)(r.report_wall_ms() - r.process_start_wall_ms()),
                  (unsigned long long)r.keys_fetched());
    return 0;
  }
  if (cmd == "faults" && args.size() >= 3) {
    const wire::Endpoint* node = FindStorageNode(*cfg, args[1]);
    if (!node) {
      std::fprintf(stderr, "unknown node %s\n", args[1].c_str());
      return 1;
    }
    wire::FaultConfig fc;
    const std::string& mode = args[2];
    if (mode == "drop-out" || mode == "drop-in") {
      if (args.size() < 4) return Usage();
      for (const auto& p : Split(args[3], ',')) {
        auto* r = fc.add_rules();
        r->set_peer(p);
        (mode == "drop-out") ? r->set_drop_outgoing(true) : r->set_drop_incoming(true);
      }
    } else if (mode == "delay") {
      if (args.size() < 5) return Usage();
      auto* r = fc.add_rules();
      r->set_peer(args[3]);
      r->set_delay_ms(uint32_t(std::stoul(args[4])));
    } else if (mode != "clear") {
      return Usage();
    }
    auto stub = wire::ReplicaService::NewStub(MakeChannel(EndpointAddress(*node)));
    grpc::ClientContext ctx;
    SetDeadlineMs(&ctx, 2000);
    wire::Result res;
    auto s = stub->SetFaults(&ctx, fc, &res);
    std::printf("%s\n", s.ok() ? "OK" : s.error_message().c_str());
    return s.ok() ? 0 : 1;
  }
  return Usage();
}
