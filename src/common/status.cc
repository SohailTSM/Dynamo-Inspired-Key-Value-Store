#include "common/status.h"

#include <cerrno>
#include <cstring>

namespace dkv {

const char* StatusCodeName(StatusCode c) {
  switch (c) {
    case StatusCode::kOk: return "OK";
    case StatusCode::kNotFound: return "NOT_FOUND";
    case StatusCode::kInvalidArgument: return "INVALID_ARGUMENT";
    case StatusCode::kUnavailable: return "UNAVAILABLE";
    case StatusCode::kTimeout: return "TIMEOUT";
    case StatusCode::kCorruption: return "CORRUPTION";
    case StatusCode::kIoError: return "IO_ERROR";
    case StatusCode::kInternal: return "INTERNAL";
    case StatusCode::kQuorumNotReached: return "QUORUM_NOT_REACHED";
    case StatusCode::kNotAReplica: return "NOT_A_REPLICA";
  }
  return "UNKNOWN";
}

std::string Status::ToString() const {
  if (ok()) return "OK";
  return std::string(StatusCodeName(code_)) + ": " + message_;
}

Status ErrnoStatus(const std::string& what) {
  int e = errno;
  return Status::IoError(what + ": " + std::strerror(e));
}

}  // namespace dkv
