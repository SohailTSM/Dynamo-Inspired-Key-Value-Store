#include "storage/record_codec.h"

#include "common/hash.h"

namespace dkv {

void PutU32LE(std::string* out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out->push_back(char((v >> (8 * i)) & 0xFF));
}

uint32_t GetU32LE(const char* p) {
  const auto* u = reinterpret_cast<const unsigned char*>(p);
  return uint32_t(u[0]) | uint32_t(u[1]) << 8 | uint32_t(u[2]) << 16 | uint32_t(u[3]) << 24;
}

std::string FrameRecord(const wire::LogRecord& rec) {
  std::string payload;
  rec.SerializeToString(&payload);
  std::string out;
  out.reserve(kFrameHeaderBytes + payload.size());
  PutU32LE(&out, uint32_t(payload.size()));
  PutU32LE(&out, Crc32c(payload));
  out += payload;
  return out;
}

ParseResult ParseFrame(const char* data, size_t avail, wire::LogRecord* rec, uint32_t* frame_len) {
  if (avail < kFrameHeaderBytes) return ParseResult::kIncomplete;
  uint32_t len = GetU32LE(data);
  uint32_t crc = GetU32LE(data + 4);
  if (len > kMaxPayloadBytes) return ParseResult::kCorrupt;
  if (avail < kFrameHeaderBytes + len) return ParseResult::kIncomplete;
  const char* payload = data + kFrameHeaderBytes;
  if (Crc32c(payload, len) != crc) return ParseResult::kCorrupt;
  if (!rec->ParseFromArray(payload, int(len))) return ParseResult::kCorrupt;
  *frame_len = uint32_t(kFrameHeaderBytes + len);
  return ParseResult::kOk;
}

}  // namespace dkv
