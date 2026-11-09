// Status / StatusOr: error handling without exceptions across module boundaries (LLD §1).
#pragma once

#include <optional>
#include <string>
#include <utility>

namespace dkv {

enum class StatusCode {
  kOk = 0,
  kNotFound,
  kInvalidArgument,
  kUnavailable,
  kTimeout,
  kCorruption,
  kIoError,
  kInternal,
  kQuorumNotReached,
  kNotAReplica,
};

const char* StatusCodeName(StatusCode c);

class Status {
 public:
  Status() = default;
  Status(StatusCode code, std::string message) : code_(code), message_(std::move(message)) {}

  static Status OK() { return Status(); }
  static Status NotFound(std::string m) { return {StatusCode::kNotFound, std::move(m)}; }
  static Status InvalidArgument(std::string m) { return {StatusCode::kInvalidArgument, std::move(m)}; }
  static Status Unavailable(std::string m) { return {StatusCode::kUnavailable, std::move(m)}; }
  static Status Timeout(std::string m) { return {StatusCode::kTimeout, std::move(m)}; }
  static Status Corruption(std::string m) { return {StatusCode::kCorruption, std::move(m)}; }
  static Status IoError(std::string m) { return {StatusCode::kIoError, std::move(m)}; }
  static Status Internal(std::string m) { return {StatusCode::kInternal, std::move(m)}; }
  static Status QuorumNotReached(std::string m) { return {StatusCode::kQuorumNotReached, std::move(m)}; }

  bool ok() const { return code_ == StatusCode::kOk; }
  StatusCode code() const { return code_; }
  const std::string& message() const { return message_; }
  std::string ToString() const;

 private:
  StatusCode code_ = StatusCode::kOk;
  std::string message_;
};

// Holds either a value or a non-OK status.
template <typename T>
class StatusOr {
 public:
  StatusOr(T value) : value_(std::move(value)) {}           // NOLINT: implicit by design
  StatusOr(Status status) : status_(std::move(status)) {     // NOLINT: implicit by design
    if (status_.ok()) status_ = Status::Internal("StatusOr constructed from OK status without value");
  }

  bool ok() const { return value_.has_value(); }
  const Status& status() const { return status_; }
  const T& value() const& { return *value_; }
  T& value() & { return *value_; }
  T&& value() && { return std::move(*value_); }
  const T& operator*() const& { return *value_; }
  T& operator*() & { return *value_; }
  const T* operator->() const { return &*value_; }
  T* operator->() { return &*value_; }

 private:
  std::optional<T> value_;
  Status status_;
};

// Errno-based helper: "what: strerror(errno)".
Status ErrnoStatus(const std::string& what);

}  // namespace dkv

#define DKV_RETURN_IF_ERROR(expr)          \
  do {                                     \
    ::dkv::Status _st = (expr);            \
    if (!_st.ok()) return _st;             \
  } while (0)
