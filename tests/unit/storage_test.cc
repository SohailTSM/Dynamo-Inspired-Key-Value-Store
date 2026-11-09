#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <thread>
#include <vector>

#include "common/fs.h"
#include "storage/record_codec.h"
#include "storage/segment.h"
#include "storage/storage_engine.h"
#include "test_framework.h"

using namespace dkv;

namespace {

std::string TempDir(const std::string& tag) { return dkvtest::NewTestDir(tag); }

wire::Version MakeVersion(const std::string& coord, uint64_t counter, const std::string& value,
                        int64_t ts = 1000) {
  VectorClock c;
  c.set(coord, counter);
  wire::Version v;
  v.set_value(value);
  c.ToProto(v.mutable_clock());
  v.set_timestamp_ms(ts);
  v.set_coordinator_id(coord);
  v.set_digest(DigestToBytes(ComputeVersionDigest(c, ts, coord, value)));
  return v;
}

void AppendRaw(const std::string& path, const std::string& bytes) {
  int fd = ::open(path.c_str(), O_WRONLY | O_APPEND);
  REQUIRE(fd >= 0);
  REQUIRE(::write(fd, bytes.data(), bytes.size()) == ssize_t(bytes.size()));
  ::close(fd);
}

}  // namespace

TEST_CASE("codec: round-trip, incomplete, corrupt") {
  wire::LogRecord rec;
  rec.set_type(kRecordPutVersion);
  rec.set_key("k1");
  *rec.mutable_version() = MakeVersion("A", 1, "hello");
  std::string f = FrameRecord(rec);
  wire::LogRecord out;
  uint32_t len = 0;
  CHECK(ParseFrame(f.data(), f.size(), &out, &len) == ParseResult::kOk);
  CHECK_EQ(len, uint32_t(f.size()));
  CHECK_EQ(out.key(), std::string("k1"));
  CHECK_EQ(out.version().value(), std::string("hello"));
  CHECK(ParseFrame(f.data(), 5, &out, &len) == ParseResult::kIncomplete);          // torn header
  CHECK(ParseFrame(f.data(), f.size() - 1, &out, &len) == ParseResult::kIncomplete);  // torn payload
  std::string bad = f;
  bad[bad.size() - 1] ^= 0x5A;
  CHECK(ParseFrame(bad.data(), bad.size(), &out, &len) == ParseResult::kCorrupt);
}

TEST_CASE("segment: file names") {
  CHECK_EQ(Segment::FileName(7), std::string("segment-000007.log"));
  uint32_t id = 0;
  CHECK(Segment::ParseFileName("segment-000042.log", &id));
  CHECK_EQ(id, 42u);
  CHECK(!Segment::ParseFileName("segment-42.log", &id));
  CHECK(!Segment::ParseFileName("node.meta", &id));
}

TEST_CASE("index: apply rules (dominated, dominating, concurrent, duplicate, equal-clock tie)") {
  Index idx;
  auto m = [](const std::string& c, uint64_t n, int64_t ts, const std::string& val) {
    return MetaFromProto(MakeVersion(c, n, val, ts));
  };
  CHECK(idx.apply("k", m("A", 1, 10, "a1")));
  CHECK(!idx.apply("k", m("A", 1, 10, "a1")));  // duplicate
  CHECK(idx.apply("k", m("A", 2, 5, "a2")));    // dominates (despite older ts)
  CHECK_EQ(idx.current("k").size(), 1u);
  CHECK(!idx.apply("k", m("A", 1, 99, "a1x")));  // dominated
  CHECK(idx.apply("k", m("B", 1, 7, "b1")));      // concurrent sibling
  CHECK_EQ(idx.current("k").size(), 2u);
  CHECK(idx.covers("k", VectorClock::FromProto(MakeVersion("A", 1, "z").clock()), Digest160{}));

  // Order independence: same set applied in reverse order gives the same current set.
  std::vector<VersionMeta> vs = {m("A", 1, 1, "x"), m("B", 1, 2, "y"), m("A", 2, 3, "z"),
                                 m("C", 1, 3, "w")};
  Index i1, i2;
  for (auto& v : vs) i1.apply("q", v);
  for (auto it = vs.rbegin(); it != vs.rend(); ++it) i2.apply("q", *it);
  auto c1 = i1.current("q"), c2 = i2.current("q");
  REQUIRE(c1.size() == c2.size());
  for (auto& a : c1) {
    bool found = false;
    for (auto& b : c2) found |= (a.digest == b.digest);
    CHECK(found);
  }
}

TEST_CASE("engine: put, read back, persistence across reopen") {
  std::string dir = TempDir("engine");
  {
    LogStorageEngine e(dir, 1 << 20);
    REQUIRE(e.open().ok());
    auto r = e.put_version("k1", MakeVersion("A", 1, "v1"));
    REQUIRE(r.ok());
    CHECK(*r);
    auto again = e.put_version("k1", MakeVersion("A", 1, "v1"));
    REQUIRE(again.ok());
    CHECK(!*again);  // idempotent
    REQUIRE(e.put_version("k1", MakeVersion("A", 2, "v2")).ok());
    REQUIRE(e.put_version("k2", MakeVersion("B", 1, "other")).ok());
    auto cur = e.current_versions("k1");
    REQUIRE(cur.size() == 1);
    auto full = e.read_version("k1", cur[0]);
    REQUIRE(full.ok());
    CHECK_EQ(full->value(), std::string("v2"));
    e.close();
  }
  LogStorageEngine e2(dir, 1 << 20);
  REQUIRE(e2.open().ok());
  CHECK_EQ(e2.recovery_stats().records, 3u);
  CHECK_EQ(e2.stats().keys, 2u);
  auto cur = e2.current_versions("k1");
  REQUIRE(cur.size() == 1);
  auto full = e2.read_version("k1", cur[0]);
  REQUIRE(full.ok());
  CHECK_EQ(full->value(), std::string("v2"));
}

TEST_CASE("engine: group commit shares fsyncs across concurrent writers") {
  std::string dir = TempDir("group");
  LogStorageEngine e(dir, 64 << 20);
  REQUIRE(e.open().ok());
  const int kThreads = 16, kPer = 100;
  std::vector<std::thread> ts;
  std::atomic<int> failures{0};
  for (int t = 0; t < kThreads; ++t)
    ts.emplace_back([&, t] {
      for (int i = 0; i < kPer; ++i) {
        auto r = e.put_version("k" + std::to_string(t) + "-" + std::to_string(i),
                               MakeVersion("A", uint64_t(i + 1), "val"));
        if (!r.ok() || !*r) failures++;
      }
    });
  for (auto& t : ts) t.join();
  CHECK_EQ(failures.load(), 0);
  auto s = e.stats();
  CHECK_EQ(s.records_appended, uint64_t(kThreads * kPer));
  CHECK(s.fsyncs < s.records_appended);  // batching happened
  CHECK_EQ(s.keys, uint64_t(kThreads * kPer));
}

TEST_CASE("engine: segment rollover and reopen across segments") {
  std::string dir = TempDir("roll");
  {
    LogStorageEngine e(dir, 4096);  // tiny segments
    REQUIRE(e.open().ok());
    for (int i = 0; i < 200; ++i)
      REQUIRE(e.put_version("key" + std::to_string(i), MakeVersion("A", 1, std::string(100, 'x'))).ok());
    e.close();
  }
  auto names = ListDir(dir);
  REQUIRE(names.ok());
  CHECK(names->size() > 3);
  LogStorageEngine e2(dir, 4096);
  REQUIRE(e2.open().ok());
  CHECK_EQ(e2.stats().keys, 200u);
  auto cur = e2.current_versions("key150");
  REQUIRE(cur.size() == 1);
  CHECK(e2.read_version("key150", cur[0]).ok());
}

TEST_CASE("recovery: torn tail in the active segment is truncated") {
  std::string dir = TempDir("torn");
  {
    LogStorageEngine e(dir, 1 << 20);
    REQUIRE(e.open().ok());
    REQUIRE(e.put_version("a", MakeVersion("A", 1, "1")).ok());
    REQUIRE(e.put_version("b", MakeVersion("A", 1, "2")).ok());
    e.close();
  }
  wire::LogRecord rec;
  rec.set_type(kRecordPutVersion);
  rec.set_key("c");
  *rec.mutable_version() = MakeVersion("A", 1, "3");
  std::string frame = FrameRecord(rec);
  AppendRaw(JoinPath(dir, Segment::FileName(1)), frame.substr(0, frame.size() / 2));  // crash mid-append
  {
    LogStorageEngine e(dir, 1 << 20);
    REQUIRE(e.open().ok());
    CHECK_EQ(e.recovery_stats().records, 2u);
    CHECK(e.recovery_stats().truncated_bytes > 0);
    CHECK(e.current_versions("c").empty());
    // The log is writable again after truncation.
    REQUIRE(e.put_version("d", MakeVersion("A", 1, "4")).ok());
    e.close();
  }
  LogStorageEngine e(dir, 1 << 20);
  REQUIRE(e.open().ok());
  CHECK_EQ(e.recovery_stats().records, 3u);
  CHECK_EQ(e.recovery_stats().truncated_bytes, 0u);
}

TEST_CASE("recovery: corruption in a sealed segment aborts startup") {
  std::string dir = TempDir("sealed");
  {
    LogStorageEngine e(dir, 2048);
    REQUIRE(e.open().ok());
    for (int i = 0; i < 60; ++i)
      REQUIRE(e.put_version("k" + std::to_string(i), MakeVersion("A", 1, std::string(80, 'y'))).ok());
    e.close();
  }
  // Flip a byte in the middle of segment 1 (sealed).
  std::string p = JoinPath(dir, Segment::FileName(1));
  int fd = ::open(p.c_str(), O_RDWR);
  REQUIRE(fd >= 0);
  char b;
  REQUIRE(::pread(fd, &b, 1, 100) == 1);
  b ^= 0x7F;
  REQUIRE(::pwrite(fd, &b, 1, 100) == 1);
  ::close(fd);
  LogStorageEngine e(dir, 2048);
  Status s = e.open();
  CHECK(!s.ok());
  CHECK(s.code() == StatusCode::kCorruption);
}

TEST_CASE("engine: change listener sees current-set changes") {
  std::string dir = TempDir("listen");
  LogStorageEngine e(dir, 1 << 20);
  int calls = 0;
  size_t last_size = 0;
  e.set_change_listener([&](const std::string&, const std::vector<VersionMeta>& cur) {
    calls++;
    last_size = cur.size();
  });
  REQUIRE(e.open().ok());
  REQUIRE(e.put_version("k", MakeVersion("A", 1, "x")).ok());
  REQUIRE(e.put_version("k", MakeVersion("B", 1, "y")).ok());
  REQUIRE(e.put_version("k", MakeVersion("A", 1, "x")).ok());  // no change
  CHECK_EQ(calls, 2);
  CHECK_EQ(last_size, 2u);
}
