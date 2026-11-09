#include "common/flags.h"

#include <cstdlib>

namespace dkv {

Flags::Flags(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a.rfind("--", 0) == 0 && a.size() > 2) {
      std::string body = a.substr(2);
      auto eq = body.find('=');
      if (eq != std::string::npos) {
        values_[body.substr(0, eq)] = body.substr(eq + 1);
      } else if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
        values_[body] = argv[++i];
      } else {
        values_[body] = "true";
      }
    } else {
      positional_.push_back(a);
    }
  }
}

std::string Flags::get(const std::string& key, const std::string& def) const {
  auto it = values_.find(key);
  return it == values_.end() ? def : it->second;
}

int64_t Flags::get_int(const std::string& key, int64_t def) const {
  auto it = values_.find(key);
  return it == values_.end() ? def : std::strtoll(it->second.c_str(), nullptr, 10);
}

double Flags::get_double(const std::string& key, double def) const {
  auto it = values_.find(key);
  return it == values_.end() ? def : std::strtod(it->second.c_str(), nullptr);
}

bool Flags::get_bool(const std::string& key, bool def) const {
  auto it = values_.find(key);
  if (it == values_.end()) return def;
  return it->second == "true" || it->second == "1" || it->second == "yes";
}

}  // namespace dkv
