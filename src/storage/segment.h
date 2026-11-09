// One segment file of the version log: 16-byte header ("DKVSEG01", u32 id, u32 0) then records.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "common/status.h"

namespace dkv {

constexpr size_t kSegmentHeaderBytes = 16;

class Segment {
 public:
  ~Segment();
  Segment(const Segment&) = delete;
  Segment& operator=(const Segment&) = delete;

  static std::string FileName(uint32_t id);              // segment-000001.log
  static bool ParseFileName(const std::string& name, uint32_t* id);

  // Creates a new file with its header, fsyncs it and the directory.
  static StatusOr<std::unique_ptr<Segment>> Create(const std::string& dir, uint32_t id);
  // Opens an existing file (header checked by the caller via ReadAll + CheckHeader).
  static StatusOr<std::unique_ptr<Segment>> Open(const std::string& dir, uint32_t id);

  StatusOr<std::string> ReadAll() const;
  static bool CheckHeader(const std::string& bytes, uint32_t id);
  Status RewriteHeader();  // for a segment whose header write was torn

  StatusOr<uint64_t> append(const std::string& bytes);  // returns start offset
  Status sync();                                        // fdatasync
  StatusOr<std::string> pread(uint64_t off, uint32_t len) const;
  Status truncate(uint64_t size);  // ftruncate + fsync

  uint64_t size() const { return size_; }
  uint32_t id() const { return id_; }
  const std::string& path() const { return path_; }

 private:
  Segment(int fd, uint32_t id, std::string path, uint64_t size)
      : fd_(fd), id_(id), path_(std::move(path)), size_(size) {}
  int fd_;
  uint32_t id_;
  std::string path_;
  uint64_t size_;  // only modified by the committer thread
};

}  // namespace dkv
