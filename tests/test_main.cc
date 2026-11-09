#include <chrono>
#include <cstring>
#include <exception>
#include <atomic>
#include <cstdlib>
#include <unistd.h>

#include "common/fs.h"

#include "common/logging.h"
#include "test_framework.h"

namespace dkvtest {

std::vector<TestCase>& Registry() {
  static std::vector<TestCase> r;
  return r;
}
int& FailureCount() {
  static int n = 0;
  return n;
}
std::string TestTmpRoot() {
  static const std::string root = [] {
    const char* base = std::getenv("DKV_TEST_TMPDIR");
    std::string b = base && *base ? base : "test-tmp";
    std::string r = dkv::JoinPath(b, std::to_string(::getpid()));
    dkv::MakeDirs(r);
    return r;
  }();
  return root;
}
std::string NewTestDir(const std::string& tag) {
  static std::atomic<int> n{0};
  std::string d = dkv::JoinPath(TestTmpRoot(), tag + "-" + std::to_string(n++));
  dkv::MakeDirs(d);
  return d;
}
void ReportFailure(const char* file, int line, const std::string& msg) {
  ++FailureCount();
  std::fprintf(stderr, "    FAIL %s:%d: %s\n", file, line, msg.c_str());
}

}  // namespace dkvtest

int main(int argc, char** argv) {
  dkv::SetLogLevel(dkv::LogLevel::WARN);
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int passed = 0, failed = 0;
  for (const auto& tc : dkvtest::Registry()) {
    if (filter && !std::strstr(tc.name, filter)) continue;
    int before = dkvtest::FailureCount();
    auto t0 = std::chrono::steady_clock::now();
    try {
      tc.fn();
    } catch (const dkvtest::RequireFailed&) {
    } catch (const std::exception& e) {
      dkvtest::ReportFailure(tc.file, tc.line, std::string("exception: ") + e.what());
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    bool ok = dkvtest::FailureCount() == before;
    std::fprintf(stderr, "[%s] %s (%.1f ms)\n", ok ? " OK " : "FAIL", tc.name, ms);
    ok ? ++passed : ++failed;
  }
  dkv::RemoveTree(dkvtest::TestTmpRoot());
  std::fprintf(stderr, "\n%d passed, %d failed\n", passed, failed);
  return failed == 0 ? 0 : 1;
}
