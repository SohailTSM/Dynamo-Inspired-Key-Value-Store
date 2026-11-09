// Storage engine interface (LLD §5.3) + the log-structured implementation.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common.pb.h"
#include "common/status.h"
#include "storage/index.h"
#include "storage/version_log.h"

namespace dkv {

struct StorageStats {
  uint64_t keys = 0;
  uint64_t records_appended = 0;
  uint64_t fsyncs = 0;
  uint64_t bytes_appended = 0;
};

class StorageEngine {
 public:
  using ChangeListener = Index::OnChange;
  virtual ~StorageEngine() = default;
  virtual Status open() = 0;
  virtual void close() = 0;
  // Durably stores `v` (fsync before return) unless it is already current or dominated.
  // Returns true if a record was appended.
  virtual StatusOr<bool> put_version(const std::string& key, const wire::Version& v) = 0;
  virtual std::vector<VersionMeta> current_versions(const std::string& key) const = 0;
  // Full version including value (pread + CRC check).
  virtual StatusOr<wire::Version> read_version(const std::string& key, const VersionMeta& meta) const = 0;
  virtual void for_each_key(const ChangeListener& visit) const = 0;
  virtual StorageStats stats() const = 0;
  // Called (under the index shard lock) whenever a key's current set changes. Set before open().
  virtual void set_change_listener(ChangeListener l) = 0;
};

class LogStorageEngine final : public StorageEngine {
 public:
  LogStorageEngine(std::string dir, uint64_t segment_bytes);
  ~LogStorageEngine() override;

  Status open() override;
  void close() override;
  StatusOr<bool> put_version(const std::string& key, const wire::Version& v) override;
  std::vector<VersionMeta> current_versions(const std::string& key) const override;
  StatusOr<wire::Version> read_version(const std::string& key, const VersionMeta& meta) const override;
  void for_each_key(const ChangeListener& visit) const override;
  StorageStats stats() const override;
  void set_change_listener(ChangeListener l) override { listener_ = std::move(l); }

  const RecoveryStats& recovery_stats() const { return recovery_; }

 private:
  VersionLog log_;
  Index index_;
  ChangeListener listener_;
  RecoveryStats recovery_;
};

}  // namespace dkv
