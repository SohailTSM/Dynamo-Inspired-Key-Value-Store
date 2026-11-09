#include "storage/segment.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "common/fs.h"
#include "storage/record_codec.h"

namespace dkv {
namespace {
const char kMagic[8] = {'D', 'K', 'V', 'S', 'E', 'G', '0', '1'};

std::string HeaderBytes(uint32_t id) {
  std::string h(kMagic, 8);
  PutU32LE(&h, id);
  PutU32LE(&h, 0);
  return h;
}

Status WriteFully(int fd, const char* p, size_t n, uint64_t off, const std::string& path) {
  while (n > 0) {
    ssize_t w = ::pwrite(fd, p, n, off_t(off));
    if (w < 0) {
      if (errno == EINTR) continue;
      return ErrnoStatus("pwrite " + path);
    }
    p += w;
    n -= size_t(w);
    off += uint64_t(w);
  }
  return Status::OK();
}
}  // namespace

Segment::~Segment() {
  if (fd_ >= 0) ::close(fd_);
}

std::string Segment::FileName(uint32_t id) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "segment-%06u.log", id);
  return buf;
}

bool Segment::ParseFileName(const std::string& name, uint32_t* id) {
  unsigned v = 0;
  char tail[8] = {0};
  if (std::sscanf(name.c_str(), "segment-%6u.%3s", &v, tail) != 2) return false;
  if (std::strcmp(tail, "log") != 0 || name != FileName(v)) return false;
  *id = v;
  return true;
}

StatusOr<std::unique_ptr<Segment>> Segment::Create(const std::string& dir, uint32_t id) {
  std::string path = JoinPath(dir, FileName(id));
  int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd < 0) return ErrnoStatus("create " + path);
  std::string h = HeaderBytes(id);
  Status s = WriteFully(fd, h.data(), h.size(), 0, path);
  if (s.ok() && ::fsync(fd) != 0) s = ErrnoStatus("fsync " + path);
  if (!s.ok()) {
    ::close(fd);
    return s;
  }
  DKV_RETURN_IF_ERROR(FsyncDir(dir));
  return std::unique_ptr<Segment>(new Segment(fd, id, path, kSegmentHeaderBytes));
}

StatusOr<std::unique_ptr<Segment>> Segment::Open(const std::string& dir, uint32_t id) {
  std::string path = JoinPath(dir, FileName(id));
  int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) return ErrnoStatus("open " + path);
  struct stat st;
  if (::fstat(fd, &st) != 0) {
    Status s = ErrnoStatus("fstat " + path);
    ::close(fd);
    return s;
  }
  return std::unique_ptr<Segment>(new Segment(fd, id, path, uint64_t(st.st_size)));
}

StatusOr<std::string> Segment::ReadAll() const { return pread(0, uint32_t(size_)); }

bool Segment::CheckHeader(const std::string& bytes, uint32_t id) {
  return bytes.size() >= kSegmentHeaderBytes && std::memcmp(bytes.data(), kMagic, 8) == 0 &&
         GetU32LE(bytes.data() + 8) == id;
}

Status Segment::RewriteHeader() {
  DKV_RETURN_IF_ERROR(truncate(0));
  std::string h = HeaderBytes(id_);
  DKV_RETURN_IF_ERROR(WriteFully(fd_, h.data(), h.size(), 0, path_));
  size_ = kSegmentHeaderBytes;
  return sync();
}

StatusOr<uint64_t> Segment::append(const std::string& bytes) {
  uint64_t off = size_;
  DKV_RETURN_IF_ERROR(WriteFully(fd_, bytes.data(), bytes.size(), off, path_));
  size_ += bytes.size();
  return off;
}

Status Segment::sync() {
  if (::fdatasync(fd_) != 0) return ErrnoStatus("fdatasync " + path_);
  return Status::OK();
}

StatusOr<std::string> Segment::pread(uint64_t off, uint32_t len) const {
  std::string out(len, '\0');
  size_t got = 0;
  while (got < len) {
    ssize_t r = ::pread(fd_, &out[got], len - got, off_t(off + got));
    if (r < 0) {
      if (errno == EINTR) continue;
      return ErrnoStatus("pread " + path_);
    }
    if (r == 0) break;
    got += size_t(r);
  }
  out.resize(got);
  return out;
}

Status Segment::truncate(uint64_t size) {
  if (::ftruncate(fd_, off_t(size)) != 0) return ErrnoStatus("ftruncate " + path_);
  if (::fsync(fd_) != 0) return ErrnoStatus("fsync " + path_);
  size_ = size;
  return Status::OK();
}

}  // namespace dkv
