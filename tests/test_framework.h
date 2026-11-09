// Minimal unit-test harness (instead of a downloaded framework; works offline on RCE).
//   TEST_CASE("name") { CHECK(a == b); REQUIRE(ok); CHECK_EQ(a, b); }
// Run: dkv_tests [substring-filter]
#pragma once

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace dkvtest {

struct TestCase {
  const char* name;
  const char* file;
  int line;
  std::function<void()> fn;
};

std::vector<TestCase>& Registry();
struct Registrar {
  Registrar(const char* name, const char* file, int line, std::function<void()> fn) {
    Registry().push_back({name, file, line, std::move(fn)});
  }
};

struct RequireFailed {};

// Per-process scratch directory for tests: $DKV_TEST_TMPDIR (default ./test-tmp)/<pid>.
// Created on first use and removed recursively when dkv_tests exits.
std::string TestTmpRoot();
std::string NewTestDir(const std::string& tag);
void ReportFailure(const char* file, int line, const std::string& msg);
int& FailureCount();

template <typename A, typename B>
std::string FormatEq(const A& a, const B& b) {
  std::ostringstream ss;
  ss << "  lhs: " << a << "\n  rhs: " << b;
  return ss.str();
}

}  // namespace dkvtest

#define DKV_TEST_CONCAT2(a, b) a##b
#define DKV_TEST_CONCAT(a, b) DKV_TEST_CONCAT2(a, b)
#define TEST_CASE(name)                                                                      \
  static void DKV_TEST_CONCAT(dkv_test_fn_, __LINE__)();                                      \
  static ::dkvtest::Registrar DKV_TEST_CONCAT(dkv_test_reg_, __LINE__)(                       \
      name, __FILE__, __LINE__, &DKV_TEST_CONCAT(dkv_test_fn_, __LINE__));                    \
  static void DKV_TEST_CONCAT(dkv_test_fn_, __LINE__)()

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) ::dkvtest::ReportFailure(__FILE__, __LINE__, "CHECK(" #cond ")"); \
  } while (0)

#define REQUIRE(cond)                                                     \
  do {                                                                    \
    if (!(cond)) {                                                        \
      ::dkvtest::ReportFailure(__FILE__, __LINE__, "REQUIRE(" #cond ")"); \
      throw ::dkvtest::RequireFailed{};                                   \
    }                                                                     \
  } while (0)

#define CHECK_EQ(a, b)                                                                     \
  do {                                                                                     \
    const auto _va = (a);                                                                    \
    const auto _vb = (b);                                                                    \
    if (!(_va == _vb))                                                                     \
      ::dkvtest::ReportFailure(__FILE__, __LINE__,                                         \
                               "CHECK_EQ(" #a ", " #b ")\n" + ::dkvtest::FormatEq(_va, _vb)); \
  } while (0)
