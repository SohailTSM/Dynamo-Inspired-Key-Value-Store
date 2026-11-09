#include "version/vector_clock.h"

#include <algorithm>

namespace dkv {

const char* OrderName(Order o) {
  switch (o) {
    case Order::kBefore: return "before";
    case Order::kAfter: return "after";
    case Order::kEqual: return "equal";
    case Order::kConcurrent: return "concurrent";
  }
  return "?";
}

void VectorClock::increment(const std::string& node) { ++entries_[node]; }

uint64_t VectorClock::get(const std::string& node) const {
  auto it = entries_.find(node);
  return it == entries_.end() ? 0 : it->second;
}

void VectorClock::set(const std::string& node, uint64_t value) {
  if (value == 0)
    entries_.erase(node);
  else
    entries_[node] = value;
}

void VectorClock::merge(const VectorClock& other) {
  for (const auto& kv : other.entries_) {
    uint64_t& mine = entries_[kv.first];
    mine = std::max(mine, kv.second);
  }
}

Order VectorClock::compare(const VectorClock& other) const {
  bool this_greater = false, other_greater = false;
  auto a = entries_.begin(), ae = entries_.end();
  auto b = other.entries_.begin(), be = other.entries_.end();
  // Merge-walk both sorted maps.
  while (a != ae || b != be) {
    if (b == be || (a != ae && a->first < b->first)) {
      this_greater = true;  // other has 0 here
      ++a;
    } else if (a == ae || b->first < a->first) {
      other_greater = true;
      ++b;
    } else {
      if (a->second > b->second) this_greater = true;
      if (b->second > a->second) other_greater = true;
      ++a;
      ++b;
    }
    if (this_greater && other_greater) return Order::kConcurrent;
  }
  if (this_greater) return Order::kAfter;
  if (other_greater) return Order::kBefore;
  return Order::kEqual;
}

std::string VectorClock::canonical_bytes() const {
  std::string s;
  for (const auto& kv : entries_) s += kv.first + ":" + std::to_string(kv.second) + ";";
  return s;
}

std::string VectorClock::ToString() const {
  std::string s = "{";
  bool first = true;
  for (const auto& kv : entries_) {
    if (!first) s += ",";
    first = false;
    s += kv.first + ":" + std::to_string(kv.second);
  }
  return s + "}";
}

VectorClock VectorClock::FromProto(const wire::VectorClock& p) {
  VectorClock c;
  for (const auto& kv : p.entries()) c.set(kv.first, kv.second);
  return c;
}

void VectorClock::ToProto(wire::VectorClock* p) const {
  p->clear_entries();
  auto* m = p->mutable_entries();
  for (const auto& kv : entries_) (*m)[kv.first] = kv.second;
}

}  // namespace dkv
