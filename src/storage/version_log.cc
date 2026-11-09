#include "storage/version_log.h"

#include <algorithm>
#include <vector>

#include "common/fs.h"
#include "common/logging.h"
#include "storage/record_codec.h"

namespace dkv {

VersionLog::VersionLog(std::string dir, uint64_t segment_bytes)
    : dir_(std::move(dir)), segment_bytes_(segment_bytes) {}

VersionLog::~VersionLog() { close(); }

Status VersionLog::open(const ReplayFn& replay, RecoveryStats* stats) {
  RecoveryStats local;
  if (!stats) stats = &local;
  DKV_RETURN_IF_ERROR(MakeDirs(dir_));
  auto names = ListDir(dir_);
  if (!names.ok()) return names.status();
  std::vector<uint32_t> ids;
  for (const auto& n : *names) {
    uint32_t id;
    if (Segment::ParseFileName(n, &id)) ids.push_back(id);
  }
  std::sort(ids.begin(), ids.end());

  std::unique_lock<std::shared_mutex> lock(seg_mu_);
  for (size_t i = 0; i < ids.size(); ++i) {
    const uint32_t id = ids[i];
    const bool last = (i + 1 == ids.size());
    auto seg = Segment::Open(dir_, id);
    if (!seg.ok()) return seg.status();
    auto bytes = (*seg)->ReadAll();
    if (!bytes.ok()) return bytes.status();
    if (!Segment::CheckHeader(*bytes, id)) {
      if (last && bytes->size() < kSegmentHeaderBytes) {
        // Crash while creating the segment: the header itself is torn. Nothing was ever acked here.
        DKV_RETURN_IF_ERROR((*seg)->RewriteHeader());
        stats->header_rewritten = true;
        segments_[id] = std::move(*seg);
        ++stats->segments;
        continue;
      }
      return Status::Corruption("bad segment header: " + (*seg)->path());
    }
    size_t off = kSegmentHeaderBytes;
    while (off < bytes->size()) {
      wire::LogRecord rec;
      uint32_t frame_len = 0;
      ParseResult pr = ParseFrame(bytes->data() + off, bytes->size() - off, &rec, &frame_len);
      if (pr == ParseResult::kOk) {
        if (replay) replay(rec, Location{id, off, frame_len});
        ++stats->records;
        off += frame_len;
        continue;
      }
      if (!last) {
        return Status::Corruption("corrupt record in sealed segment " + (*seg)->path() +
                                  " at offset " + std::to_string(off));
      }
      // Torn tail of the active segment: the crash happened mid-append, before the fsync that would
      // have acknowledged it. Cut it off.
      stats->truncated_bytes = bytes->size() - off;
      DKV_LOG(WARN) << "recovery: truncating torn tail of " << (*seg)->path() << " at " << off
                    << " (" << stats->truncated_bytes << " bytes)";
      DKV_RETURN_IF_ERROR((*seg)->truncate(off));
      break;
    }
    segments_[id] = std::move(*seg);
    ++stats->segments;
  }
  if (segments_.empty()) {
    auto seg = Segment::Create(dir_, 1);
    if (!seg.ok()) return seg.status();
    segments_[1] = std::move(*seg);
  }
  active_ = segments_.rbegin()->second.get();
  if (active_->size() >= segment_bytes_) DKV_RETURN_IF_ERROR(rollover_locked());
  lock.unlock();

  {
    std::lock_guard<std::mutex> l(q_mu_);
    stopping_ = false;
    open_ = true;
  }
  committer_ = std::thread([this] { commit_loop(); });
  return Status::OK();
}

void VersionLog::close() {
  {
    std::lock_guard<std::mutex> l(q_mu_);
    if (!open_) return;
    stopping_ = true;
  }
  q_cv_.notify_all();
  if (committer_.joinable()) committer_.join();
  std::lock_guard<std::mutex> l(q_mu_);
  open_ = false;
}

std::future<StatusOr<Location>> VersionLog::append(const wire::LogRecord& rec) {
  Pending p;
  p.frame = FrameRecord(rec);
  auto fut = p.done.get_future();
  {
    std::lock_guard<std::mutex> l(q_mu_);
    if (!open_ || stopping_) {
      p.done.set_value(Status::Unavailable("version log is closed"));
      return fut;
    }
    queue_.push_back(std::move(p));
  }
  q_cv_.notify_one();
  return fut;
}

void VersionLog::commit_loop() {
  for (;;) {
    std::deque<Pending> batch;
    {
      std::unique_lock<std::mutex> l(q_mu_);
      q_cv_.wait(l, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty() && stopping_) return;
      batch.swap(queue_);  // natural batching: everything that arrived during the last fsync
    }
    std::string bytes;
    std::vector<uint32_t> lens;
    lens.reserve(batch.size());
    for (auto& p : batch) {
      lens.push_back(uint32_t(p.frame.size()));
      bytes += p.frame;
    }
    // Only this thread modifies active_ / its size, so no lock is needed for the write itself.
    Segment* seg = active_;
    auto off = seg->append(bytes);
    Status st = off.ok() ? seg->sync() : off.status();
    if (st.ok()) {
      fsyncs_++;
      appended_ += batch.size();
      bytes_ += bytes.size();
    }
    uint64_t cur = off.ok() ? *off : 0;
    for (size_t i = 0; i < batch.size(); ++i) {
      if (st.ok()) {
        batch[i].done.set_value(Location{seg->id(), cur, lens[i]});
        cur += lens[i];
      } else {
        batch[i].done.set_value(st);
      }
    }
    if (!st.ok()) DKV_LOG(ERROR) << "version log write failed: " << st.ToString();
    if (seg->size() >= segment_bytes_) {
      std::unique_lock<std::shared_mutex> lock(seg_mu_);
      Status rs = rollover_locked();
      if (!rs.ok()) DKV_LOG(ERROR) << "segment rollover failed: " << rs.ToString();
    }
  }
}

Status VersionLog::rollover_locked() {
  uint32_t next = active_->id() + 1;
  auto seg = Segment::Create(dir_, next);  // header + fsync + dir fsync
  if (!seg.ok()) return seg.status();
  active_ = seg->get();
  segments_[next] = std::move(*seg);
  return Status::OK();
}

StatusOr<wire::LogRecord> VersionLog::read(const Location& loc) const {
  std::shared_lock<std::shared_mutex> lock(seg_mu_);
  auto it = segments_.find(loc.segment_id);
  if (it == segments_.end()) return Status::NotFound("no segment " + std::to_string(loc.segment_id));
  auto bytes = it->second->pread(loc.offset, loc.length);
  if (!bytes.ok()) return bytes.status();
  wire::LogRecord rec;
  uint32_t frame_len = 0;
  ParseResult pr = ParseFrame(bytes->data(), bytes->size(), &rec, &frame_len);
  if (pr != ParseResult::kOk || frame_len != loc.length)
    return Status::Corruption("record CRC/length mismatch at segment " +
                              std::to_string(loc.segment_id) + " offset " + std::to_string(loc.offset));
  return rec;
}

}  // namespace dkv
