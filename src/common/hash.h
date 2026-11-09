// MD5 (ring tokens), SHA-1 (digests), CRC32C (log records). Self-contained so no OpenSSL dependency
// (LLD §1). MD5/SHA-1 are used for spread and change detection, not security.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace dkv {

using Digest128 = std::array<uint8_t, 16>;
using Digest160 = std::array<uint8_t, 20>;

Digest128 Md5(std::string_view data);

class Sha1 {
 public:
  Sha1();
  void update(std::string_view data);
  void update(const void* data, size_t len);
  Digest160 finish();

 private:
  void process_block(const uint8_t* block);
  uint32_t h_[5];
  uint8_t buf_[64];
  size_t buf_len_ = 0;
  uint64_t total_len_ = 0;
};

Digest160 Sha1Of(std::string_view data);

uint32_t Crc32c(const void* data, size_t len, uint32_t crc = 0);
inline uint32_t Crc32c(std::string_view s) { return Crc32c(s.data(), s.size()); }

std::string ToHex(const uint8_t* data, size_t len);
template <size_t N>
std::string ToHex(const std::array<uint8_t, N>& d) {
  return ToHex(d.data(), N);
}
inline std::string DigestToBytes(const Digest160& d) { return std::string(d.begin(), d.end()); }
Digest160 BytesToDigest(std::string_view s);  // zero-padded/truncated to 20 bytes

}  // namespace dkv
