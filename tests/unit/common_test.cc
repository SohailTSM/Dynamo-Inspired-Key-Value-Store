#include <atomic>
#include <unistd.h>

#include "common/config_loader.h"
#include "common/flags.h"
#include "common/fs.h"
#include "common/hash.h"
#include "common/thread_pool.h"
#include "test_framework.h"

using namespace dkv;

TEST_CASE("hash: MD5 test vectors") {
  CHECK_EQ(ToHex(Md5("")), std::string("d41d8cd98f00b204e9800998ecf8427e"));
  CHECK_EQ(ToHex(Md5("abc")), std::string("900150983cd24fb0d6963f7d28e17f72"));
  CHECK_EQ(ToHex(Md5("The quick brown fox jumps over the lazy dog")),
           std::string("9e107d9d372bb6826bd81d3542a419d6"));
  // 56..63 byte inputs exercise the two-block padding path.
  CHECK_EQ(ToHex(Md5("12345678901234567890123456789012345678901234567890123456789012345678901234567890")),
           std::string("57edf4a22be3c955ac49da2e2107b67a"));
}

TEST_CASE("hash: SHA-1 test vectors") {
  CHECK_EQ(ToHex(Sha1Of("")), std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
  CHECK_EQ(ToHex(Sha1Of("abc")), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
  CHECK_EQ(ToHex(Sha1Of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
           std::string("84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
  Sha1 inc;  // incremental == one-shot
  inc.update("abcdbcdecdefdefgefgh");
  inc.update("fghighijhijkijkljklmklmnlmnomnopnopq");
  CHECK_EQ(ToHex(inc.finish()), std::string("84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
}

TEST_CASE("hash: CRC32C test vectors") {
  CHECK_EQ(Crc32c("123456789"), 0xE3069283u);
  std::string zeros(32, '\0');
  CHECK_EQ(Crc32c(zeros), 0x8A9136AAu);
  // Incremental computation matches one-shot.
  uint32_t part = Crc32c("12345", 5);
  CHECK_EQ(Crc32c("6789", 4, part), 0xE3069283u);
}

TEST_CASE("flags: parsing") {
  const char* argv[] = {"prog", "--id=A", "--config", "c.json", "--verbose=true", "put", "k", "v"};
  Flags f(8, const_cast<char**>(argv));
  CHECK_EQ(f.get("id"), std::string("A"));
  CHECK_EQ(f.get("config"), std::string("c.json"));
  CHECK(f.get_bool("verbose", false));
  REQUIRE(f.positional().size() == 3);
  CHECK_EQ(f.positional()[0], std::string("put"));
  CHECK_EQ(f.get_int("missing", 7), 7);
}

TEST_CASE("fs: expand vars and atomic write") {
  CHECK_EQ(ExpandVars("/tmp/${ID}/x", {{"ID", "B"}}), std::string("/tmp/B/x"));
  std::string dir = dkvtest::NewTestDir("fs");
  REQUIRE(MakeDirs(dir + "/a/b").ok());
  std::string p = dir + "/a/b/f.txt";
  REQUIRE(WriteFileAtomic(p, "hello").ok());
  auto r = ReadFile(p);
  REQUIRE(r.ok());
  CHECK_EQ(*r, std::string("hello"));
  CHECK(RandomId().size() == 32);
  CHECK(RandomId() != RandomId());
}

TEST_CASE("config: parse, defaults, validation") {
  const char* json = R"({
    "storageNodes": [{"id":"A","host":"h","port":1},{"id":"B","host":"h","port":2},
                     {"id":"C","host":"h","port":3}],
    "quorum": {"n":3,"r":2,"w":2}
  })";
  auto cfg = ParseClusterConfig(json);
  REQUIRE(cfg.ok());
  CHECK_EQ(cfg->vnodes_per_node(), 64u);
  CHECK_EQ(cfg->timing().heartbeat_interval_ms(), 1000u);
  CHECK_EQ(cfg->timing().down_after_missed(), 3u);
  CHECK_EQ(EndpointAddress(cfg->storage_nodes(1)), std::string("h:2"));

  // R + W <= N is rejected in Phase 1.
  auto bad = ParseClusterConfig(R"({"storageNodes":[{"id":"A","host":"h","port":1},
      {"id":"B","host":"h","port":2},{"id":"C","host":"h","port":3}],"quorum":{"n":3,"r":1,"w":1}})");
  CHECK(!bad.ok());
  // Duplicate ids rejected.
  auto dup = ParseClusterConfig(R"({"storageNodes":[{"id":"A","host":"h","port":1},
      {"id":"A","host":"h","port":2}],"quorum":{"n":1,"r":1,"w":1}})");
  CHECK(!dup.ok());
  // Unknown field rejected (catches typos in config files).
  auto typo = ParseClusterConfig(R"({"storageNodez":[]})");
  CHECK(!typo.ok());
}

TEST_CASE("thread_pool: runs all tasks") {
  std::atomic<int> n{0};
  {
    ThreadPool pool(4);
    for (int i = 0; i < 1000; ++i) pool.submit([&] { n++; });
    pool.stop();
  }
  CHECK_EQ(n.load(), 1000);
}

#include "tools/workload.h"
TEST_CASE("workload: deterministic values and seq parsing") {
  std::string v = WorkloadValue("k12", 7, 256);
  CHECK_EQ(v.size(), size_t(256));
  CHECK(v == WorkloadValue("k12", 7, 256));
  uint64_t s = 0;
  CHECK(ParseWorkloadSeq("k12", v, &s));
  CHECK_EQ(s, uint64_t(7));
  CHECK(!ParseWorkloadSeq("k1", v, &s));       // different key with same prefix
  std::string tiny = WorkloadValue("key", 123456, 4);  // header longer than size: kept whole
  CHECK(ParseWorkloadSeq("key", tiny, &s));
  CHECK_EQ(s, uint64_t(123456));
}
