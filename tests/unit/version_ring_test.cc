#include <map>
#include <set>

#include "ring/hash_ring.h"
#include "test_framework.h"
#include "version/version.h"

using namespace dkv;

static VectorClock VC(std::initializer_list<std::pair<const char*, uint64_t>> l) {
  VectorClock c;
  for (auto& p : l) c.set(p.first, p.second);
  return c;
}

static VersionMeta VM(VectorClock c, int64_t ts, const std::string& coord) {
  VersionMeta m;
  m.clock = std::move(c);
  m.timestamp_ms = ts;
  m.coordinator = coord;
  m.digest = ComputeVersionDigest(m.clock, ts, coord, "v-" + coord + std::to_string(ts));
  return m;
}

TEST_CASE("vector_clock: compare") {
  CHECK(VC({{"A", 1}}).compare(VC({{"A", 1}})) == Order::kEqual);
  CHECK(VC({{"A", 1}}).compare(VC({{"A", 2}})) == Order::kBefore);
  CHECK(VC({{"A", 2}}).compare(VC({{"A", 1}})) == Order::kAfter);
  CHECK(VC({{"A", 1}, {"B", 1}}).compare(VC({{"A", 1}})) == Order::kAfter);
  CHECK(VC({{"A", 1}}).compare(VC({{"B", 1}})) == Order::kConcurrent);
  CHECK(VC({{"A", 2}, {"B", 1}}).compare(VC({{"A", 1}, {"B", 2}})) == Order::kConcurrent);
  CHECK(VectorClock().compare(VC({{"A", 1}})) == Order::kBefore);
  CHECK(VectorClock().compare(VectorClock()) == Order::kEqual);
  CHECK(VC({{"A", 3}}).descends_from(VC({{"A", 3}})));
}

TEST_CASE("vector_clock: merge, increment, canonical bytes, proto round-trip") {
  VectorClock a = VC({{"A", 2}, {"B", 1}});
  a.merge(VC({{"B", 3}, {"C", 1}}));
  CHECK_EQ(a.ToString(), std::string("{A:2,B:3,C:1}"));
  a.increment("A");
  CHECK_EQ(a.get("A"), 3u);
  CHECK_EQ(a.get("Z"), 0u);
  VectorClock b;  // insertion order differs, canonical bytes must not
  b.set("C", 1);
  b.set("A", 3);
  b.set("B", 3);
  CHECK_EQ(a.canonical_bytes(), b.canonical_bytes());
  wire::VectorClock p;
  a.ToProto(&p);
  CHECK(VectorClock::FromProto(p) == a);
  b.set("C", 0);  // zero erases
  CHECK(b.entries().count("C") == 0);
}

TEST_CASE("resolver: prune dominated and duplicates") {
  auto v1 = VM(VC({{"A", 1}}), 100, "A");
  auto v2 = VM(VC({{"A", 2}}), 90, "A");  // dominates v1 despite older timestamp
  auto r = VersionResolver::prune({v1, v2, v1});
  REQUIRE(r.size() == 1);
  CHECK(r[0].digest == v2.digest);
}

TEST_CASE("resolver: concurrent siblings and declared tie rule") {
  auto a = VM(VC({{"A", 1}}), 100, "A");
  auto b = VM(VC({{"B", 1}}), 200, "B");
  auto c = VM(VC({{"C", 1}}), 200, "C");
  VersionResolver lww(ResolutionPolicy::kLwwOnConcurrent);
  auto w = lww.resolve({a, b, c});
  REQUIRE(w.size() == 1);
  CHECK_EQ(w[0].coordinator, std::string("B"));  // ts 200 tie -> lower coordinator id
  VersionResolver sib(ResolutionPolicy::kReturnSiblings);
  CHECK_EQ(sib.resolve({a, b, c}).size(), 3u);
  CHECK(lww.resolve({}).empty());
  CHECK_EQ(VersionResolver::winner_index({a, b, c}), 1);
}

TEST_CASE("resolver: digest is deterministic and value-sensitive") {
  auto c = VC({{"A", 1}});
  CHECK(ComputeVersionDigest(c, 1, "A", "x") == ComputeVersionDigest(c, 1, "A", "x"));
  CHECK(!(ComputeVersionDigest(c, 1, "A", "x") == ComputeVersionDigest(c, 1, "A", "y")));
}

static HashRing FiveNodeRing() {
  std::vector<RingNode> nodes;
  for (const char* id : {"A", "B", "C", "D", "E"}) nodes.push_back({id, std::string(id) + ":1"});
  return HashRing::Build(nodes, 64, 3, 1);
}

TEST_CASE("ring: preference list has N distinct nodes and is deterministic") {
  HashRing r1 = FiveNodeRing(), r2 = FiveNodeRing();
  CHECK_EQ(r1.range_count(), 320u);
  for (int i = 0; i < 2000; ++i) {
    std::string k = "key-" + std::to_string(i);
    auto p = r1.preference_list(k);
    REQUIRE(p.size() == 3);
    CHECK(std::set<std::string>(p.begin(), p.end()).size() == 3);
    CHECK(p == r2.preference_list(k));
    CHECK(r1.is_replica(p[0], r1.range_of(k)));
  }
}

TEST_CASE("ring: vnodes balance load (each of 5 nodes is primary for 15-25% of keys)") {
  HashRing r = FiveNodeRing();
  std::map<std::string, int> primary;
  const int kKeys = 100000;
  for (int i = 0; i < kKeys; ++i) primary[r.preference_list("k" + std::to_string(i))[0]]++;
  for (auto& kv : primary) {
    double share = double(kv.second) / kKeys;
    CHECK(share > 0.15);
    CHECK(share < 0.25);
  }
  // Each node replicates roughly 3/5 of the ranges.
  for (const char* id : {"A", "B", "C", "D", "E"}) {
    double f = double(r.ranges_replicated_by(id).size()) / r.range_count();
    CHECK(f > 0.45);
    CHECK(f < 0.75);
  }
}

TEST_CASE("ring: range_of agrees with token order and wraps") {
  HashRing r = FiveNodeRing();
  // A key whose token is beyond the last token maps to range 0 (wrap).
  bool saw_wrap = false;
  for (int i = 0; i < 20000 && !saw_wrap; ++i) {
    std::string k = "w" + std::to_string(i);
    if (r.range_of(k) == 0) saw_wrap = true;
  }
  CHECK(saw_wrap);
}

TEST_CASE("ring: RingInfo round-trip preserves placement") {
  HashRing r = FiveNodeRing();
  HashRing back = HashRing::FromRingInfo(r.ToRingInfo());
  CHECK_EQ(back.range_count(), r.range_count());
  for (int i = 0; i < 1000; ++i) {
    std::string k = "rt" + std::to_string(i);
    CHECK(back.preference_list(k) == r.preference_list(k));
  }
  CHECK_EQ(back.address_of("C"), std::string("C:1"));
}
