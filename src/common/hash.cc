#include "common/hash.h"

#include <algorithm>
#include <cstring>

namespace dkv {
namespace {

inline uint32_t rotl(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

// ---------------------------------------------------------------- MD5 (RFC 1321)
const uint32_t kMd5K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
const int kMd5S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                       5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                       4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                       6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

void md5_block(uint32_t st[4], const uint8_t* p) {
  uint32_t m[16];
  for (int i = 0; i < 16; ++i)
    m[i] = uint32_t(p[i * 4]) | uint32_t(p[i * 4 + 1]) << 8 | uint32_t(p[i * 4 + 2]) << 16 |
           uint32_t(p[i * 4 + 3]) << 24;
  uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
  for (int i = 0; i < 64; ++i) {
    uint32_t f;
    int g;
    if (i < 16) {
      f = (b & c) | (~b & d);
      g = i;
    } else if (i < 32) {
      f = (d & b) | (~d & c);
      g = (5 * i + 1) % 16;
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = (3 * i + 5) % 16;
    } else {
      f = c ^ (b | ~d);
      g = (7 * i) % 16;
    }
    uint32_t tmp = d;
    d = c;
    c = b;
    b = b + rotl(a + f + kMd5K[i] + m[g], kMd5S[i]);
    a = tmp;
  }
  st[0] += a;
  st[1] += b;
  st[2] += c;
  st[3] += d;
}

// ---------------------------------------------------------------- CRC32C (Castagnoli, reflected)
struct Crc32cTable {
  uint32_t t[256];
  Crc32cTable() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : (c >> 1);
      t[i] = c;
    }
  }
};
const Crc32cTable& crc_table() {
  static const Crc32cTable tbl;
  return tbl;
}

}  // namespace

Digest128 Md5(std::string_view data) {
  uint32_t st[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
  const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data());
  size_t n = data.size();
  size_t full = n / 64;
  for (size_t i = 0; i < full; ++i) md5_block(st, p + i * 64);
  uint8_t tail[128] = {0};
  size_t rem = n % 64;
  std::memcpy(tail, p + full * 64, rem);
  tail[rem] = 0x80;
  size_t tail_len = (rem < 56) ? 64 : 128;
  uint64_t bits = uint64_t(n) * 8;
  for (int i = 0; i < 8; ++i) tail[tail_len - 8 + i] = uint8_t(bits >> (8 * i));
  md5_block(st, tail);
  if (tail_len == 128) md5_block(st, tail + 64);
  Digest128 out;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) out[i * 4 + j] = uint8_t(st[i] >> (8 * j));
  return out;
}

// ---------------------------------------------------------------- SHA-1 (FIPS 180-4)
Sha1::Sha1() : h_{0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0} {}

void Sha1::process_block(const uint8_t* p) {
  uint32_t w[80];
  for (int i = 0; i < 16; ++i)
    w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 |
           uint32_t(p[i * 4 + 3]);
  for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
  for (int i = 0; i < 80; ++i) {
    uint32_t f, k;
    if (i < 20) {
      f = (b & c) | (~b & d);
      k = 0x5A827999;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDC;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6;
    }
    uint32_t tmp = rotl(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = rotl(b, 30);
    b = a;
    a = tmp;
  }
  h_[0] += a;
  h_[1] += b;
  h_[2] += c;
  h_[3] += d;
  h_[4] += e;
}

void Sha1::update(const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  total_len_ += len;
  while (len > 0) {
    size_t take = std::min(len, sizeof(buf_) - buf_len_);
    std::memcpy(buf_ + buf_len_, p, take);
    buf_len_ += take;
    p += take;
    len -= take;
    if (buf_len_ == 64) {
      process_block(buf_);
      buf_len_ = 0;
    }
  }
}

void Sha1::update(std::string_view data) { update(data.data(), data.size()); }

Digest160 Sha1::finish() {
  uint64_t bits = total_len_ * 8;
  uint8_t pad = 0x80;
  update(&pad, 1);
  uint8_t zero = 0;
  while (buf_len_ != 56) update(&zero, 1);
  uint8_t len_be[8];
  for (int i = 0; i < 8; ++i) len_be[i] = uint8_t(bits >> (56 - 8 * i));
  update(len_be, 8);
  Digest160 out;
  for (int i = 0; i < 5; ++i)
    for (int j = 0; j < 4; ++j) out[i * 4 + j] = uint8_t(h_[i] >> (24 - 8 * j));
  return out;
}

Digest160 Sha1Of(std::string_view data) {
  Sha1 s;
  s.update(data);
  return s.finish();
}

uint32_t Crc32c(const void* data, size_t len, uint32_t crc) {
  const auto& t = crc_table().t;
  const uint8_t* p = static_cast<const uint8_t*>(data);
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) crc = t[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

std::string ToHex(const uint8_t* data, size_t len) {
  static const char* kHex = "0123456789abcdef";
  std::string s(len * 2, '0');
  for (size_t i = 0; i < len; ++i) {
    s[2 * i] = kHex[data[i] >> 4];
    s[2 * i + 1] = kHex[data[i] & 0xF];
  }
  return s;
}

Digest160 BytesToDigest(std::string_view s) {
  Digest160 d{};
  std::memcpy(d.data(), s.data(), std::min(s.size(), d.size()));
  return d;
}

}  // namespace dkv
