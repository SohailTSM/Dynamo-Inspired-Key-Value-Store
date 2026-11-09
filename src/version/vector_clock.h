// Vector clock: node id -> counter (LLD §5.1). Zero counters are never stored, so "missing" == 0.
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "common.pb.h"

namespace dkv {

enum class Order { kBefore, kAfter, kEqual, kConcurrent };
const char* OrderName(Order o);

class VectorClock {
 public:
  VectorClock() = default;

  void increment(const std::string& node);
  uint64_t get(const std::string& node) const;
  void set(const std::string& node, uint64_t value);  // 0 erases
  void merge(const VectorClock& other);               // element-wise max
  Order compare(const VectorClock& other) const;      // this vs other
  bool descends_from(const VectorClock& other) const {
    Order o = compare(other);
    return o == Order::kAfter || o == Order::kEqual;
  }
  bool empty() const { return entries_.empty(); }
  const std::map<std::string, uint64_t>& entries() const { return entries_; }

  std::string canonical_bytes() const;  // "A:3;B:1;" sorted by id (map order)
  std::string ToString() const;         // "{A:3,B:1}"

  static VectorClock FromProto(const wire::VectorClock& p);
  void ToProto(wire::VectorClock* p) const;

  bool operator==(const VectorClock& o) const { return entries_ == o.entries_; }
  bool operator!=(const VectorClock& o) const { return !(*this == o); }

 private:
  std::map<std::string, uint64_t> entries_;  // ordered -> canonical iteration
};

}  // namespace dkv
