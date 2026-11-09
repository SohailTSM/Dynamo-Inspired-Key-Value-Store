#include "common/fs.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <random>

#include "common/hash.h"

namespace dkv {

Status MakeDirs(const std::string& path) {
  if (path.empty()) return Status::OK();
  std::string cur;
  size_t pos = 0;
  while (pos != std::string::npos) {
    pos = path.find('/', pos + 1);
    cur = path.substr(0, pos);
    if (cur.empty()) continue;
    if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return ErrnoStatus("mkdir " + cur);
  }
  return Status::OK();
}

StatusOr<std::string> ReadFile(const std::string& path) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return ErrnoStatus("open " + path);
  std::string out;
  char buf[65536];
  for (;;) {
    ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) continue;
      Status s = ErrnoStatus("read " + path);
      ::close(fd);
      return s;
    }
    if (n == 0) break;
    out.append(buf, size_t(n));
  }
  ::close(fd);
  return out;
}

Status FsyncDir(const std::string& dir) {
  int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return ErrnoStatus("open dir " + dir);
  int rc = ::fsync(fd);
  Status s = rc == 0 ? Status::OK() : ErrnoStatus("fsync dir " + dir);
  ::close(fd);
  return s;
}

Status WriteFileAtomic(const std::string& path, const std::string& contents) {
  std::string tmp = path + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return ErrnoStatus("open " + tmp);
  size_t off = 0;
  while (off < contents.size()) {
    ssize_t n = ::write(fd, contents.data() + off, contents.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      Status s = ErrnoStatus("write " + tmp);
      ::close(fd);
      return s;
    }
    off += size_t(n);
  }
  if (::fsync(fd) != 0) {
    Status s = ErrnoStatus("fsync " + tmp);
    ::close(fd);
    return s;
  }
  ::close(fd);
  if (::rename(tmp.c_str(), path.c_str()) != 0) return ErrnoStatus("rename " + tmp);
  return FsyncDir(DirName(path));
}

StatusOr<std::vector<std::string>> ListDir(const std::string& dir) {
  DIR* d = ::opendir(dir.c_str());
  if (!d) return ErrnoStatus("opendir " + dir);
  std::vector<std::string> names;
  while (dirent* e = ::readdir(d)) {
    std::string n = e->d_name;
    if (n != "." && n != "..") names.push_back(n);
  }
  ::closedir(d);
  return names;
}

bool FileExists(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
}

Status RemoveTree(const std::string& path) {
  if (path.empty() || path == "/" || path == "." || path == "..")
    return Status::InvalidArgument("refusing to remove '" + path + "'");
  struct stat st;
  if (::lstat(path.c_str(), &st) != 0) return errno == ENOENT ? Status::OK() : ErrnoStatus("lstat " + path);
  if (S_ISDIR(st.st_mode)) {
    auto names = ListDir(path);
    if (!names.ok()) return names.status();
    for (const auto& n : *names) DKV_RETURN_IF_ERROR(RemoveTree(JoinPath(path, n)));
    if (::rmdir(path.c_str()) != 0) return ErrnoStatus("rmdir " + path);
  } else if (::unlink(path.c_str()) != 0) {
    return ErrnoStatus("unlink " + path);
  }
  return Status::OK();
}

std::string DirName(const std::string& path) {
  auto pos = path.find_last_of('/');
  if (pos == std::string::npos) return ".";
  if (pos == 0) return "/";
  return path.substr(0, pos);
}

std::string JoinPath(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  if (a.back() == '/') return a + b;
  return a + "/" + b;
}

std::string ExpandVars(const std::string& in,
                       const std::vector<std::pair<std::string, std::string>>& extra) {
  std::string out;
  size_t i = 0;
  while (i < in.size()) {
    if (in[i] == '$' && i + 1 < in.size() && in[i + 1] == '{') {
      auto end = in.find('}', i + 2);
      if (end != std::string::npos) {
        std::string name = in.substr(i + 2, end - i - 2);
        bool found = false;
        for (const auto& kv : extra) {
          if (kv.first == name) {
            out += kv.second;
            found = true;
            break;
          }
        }
        if (!found) {
          const char* v = std::getenv(name.c_str());
          if (v) out += v;
        }
        i = end + 1;
        continue;
      }
    }
    out += in[i++];
  }
  return out;
}

std::string RandomId() {
  thread_local std::mt19937_64 rng(std::random_device{}() ^
                                   (uint64_t(::getpid()) << 32) ^
                                   uint64_t(reinterpret_cast<uintptr_t>(&rng)));
  uint8_t b[16];
  for (int i = 0; i < 2; ++i) {
    uint64_t v = rng();
    for (int j = 0; j < 8; ++j) b[i * 8 + j] = uint8_t(v >> (8 * j));
  }
  return ToHex(b, 16);
}

}  // namespace dkv
