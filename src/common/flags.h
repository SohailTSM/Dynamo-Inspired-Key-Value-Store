// Tiny command-line parser: --key=value, --key value, --flag (boolean), positional args.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dkv {

class Flags {
 public:
  Flags(int argc, char** argv);
  bool has(const std::string& key) const { return values_.count(key) > 0; }
  std::string get(const std::string& key, const std::string& def = "") const;
  int64_t get_int(const std::string& key, int64_t def) const;
  double get_double(const std::string& key, double def) const;
  bool get_bool(const std::string& key, bool def) const;
  const std::vector<std::string>& positional() const { return positional_; }

 private:
  std::map<std::string, std::string> values_;
  std::vector<std::string> positional_;
};

}  // namespace dkv
