#include "storage/record_codec.h"
#include "storage/storage_engine.h"

namespace dkv {

LogStorageEngine::LogStorageEngine(std::string dir, uint64_t segment_bytes)
    : log_(std::move(dir), segment_bytes) {}

LogStorageEngine::~LogStorageEngine() { close(); }

Status LogStorageEngine::open() {
  // Replay without the listener: the Merkle forest is rebuilt from the index after recovery.
  return log_.open(
      [this](const wire::LogRecord& rec, const Location& loc) {
        if (rec.type() != kRecordPutVersion) return;
        index_.apply(rec.key(), MetaFromProto(rec.version(), loc));
      },
      &recovery_);
}

void LogStorageEngine::close() { log_.close(); }

StatusOr<bool> LogStorageEngine::put_version(const std::string& key, const wire::Version& v) {
  VersionMeta meta = MetaFromProto(v);
  // Optimisation only: a racing duplicate append is harmless (Index::apply ignores it).
  if (index_.covers(key, meta.clock, meta.digest)) return false;
  wire::LogRecord rec;
  rec.set_type(kRecordPutVersion);
  rec.set_key(key);
  *rec.mutable_version() = v;
  auto loc = log_.append(rec).get();  // waits for the group-commit fsync
  if (!loc.ok()) return loc.status();
  meta.loc = *loc;
  index_.apply(key, meta, listener_);
  return true;
}

std::vector<VersionMeta> LogStorageEngine::current_versions(const std::string& key) const {
  return index_.current(key);
}

StatusOr<wire::Version> LogStorageEngine::read_version(const std::string& key,
                                                     const VersionMeta& meta) const {
  auto rec = log_.read(meta.loc);
  if (!rec.ok()) return rec.status();
  if (rec->key() != key) return Status::Corruption("record key mismatch for " + key);
  return rec->version();
}

void LogStorageEngine::for_each_key(const ChangeListener& visit) const { index_.for_each(visit); }

StorageStats LogStorageEngine::stats() const {
  StorageStats s;
  s.keys = index_.key_count();
  s.records_appended = log_.records_appended();
  s.fsyncs = log_.fsync_count();
  s.bytes_appended = log_.bytes_appended();
  return s;
}

}  // namespace dkv
