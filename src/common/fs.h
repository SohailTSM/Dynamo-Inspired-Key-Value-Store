// POSIX file helpers (no std::filesystem: GCC 8.5 on RCE needs -lstdc++fs for it).
#pragma once

#include <string>
#include <vector>

#include "common/status.h"

namespace dkv {

Status MakeDirs(const std::string& path);                 // mkdir -p
StatusOr<std::string> ReadFile(const std::string& path);
// Durable replace: write path.tmp, fsync, rename over path, fsync the directory.
Status WriteFileAtomic(const std::string& path, const std::string& contents);
Status FsyncDir(const std::string& dir);
StatusOr<std::vector<std::string>> ListDir(const std::string& dir);  // names only, no . / ..
bool FileExists(const std::string& path);
// Recursively deletes `path` (file or directory). Refuses "", "/" and relative "." / "..".
Status RemoveTree(const std::string& path);
std::string DirName(const std::string& path);
std::string JoinPath(const std::string& a, const std::string& b);

// Expands ${NAME} from the environment, plus extra (NAME, value) pairs given by the caller.
std::string ExpandVars(const std::string& in,
                       const std::vector<std::pair<std::string, std::string>>& extra = {});

std::string RandomId();  // 128-bit random hex string (request ids)

}  // namespace dkv
