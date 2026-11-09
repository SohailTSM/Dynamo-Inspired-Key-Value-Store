// Log record framing (LLD §6.2):  [u32 LE length][u32 LE crc32c(payload)][payload = wire::LogRecord]
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "common.pb.h"

namespace dkv {

constexpr size_t kFrameHeaderBytes = 8;
constexpr uint32_t kMaxPayloadBytes = 64u << 20;  // anything larger is treated as corruption
constexpr uint32_t kRecordPutVersion = 1;

std::string FrameRecord(const wire::LogRecord& rec);

enum class ParseResult { kOk, kIncomplete, kCorrupt };

// Parses one frame at `data` (with `avail` bytes available). On kOk fills rec and frame_len.
ParseResult ParseFrame(const char* data, size_t avail, wire::LogRecord* rec, uint32_t* frame_len);

void PutU32LE(std::string* out, uint32_t v);
uint32_t GetU32LE(const char* p);

}  // namespace dkv
