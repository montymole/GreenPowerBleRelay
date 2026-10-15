#pragma once
#include <cstring>
#include <map>
#include <string>
#include <vector>
class Preferences {
 public:
  inline static std::map<std::string, std::vector<unsigned char>> data;
  inline static bool failWrite = false;
  bool begin(const char *, bool) { return true; }
  void end() {}
  bool isKey(const char *key) { return data.count(key); }
  size_t getBytesLength(const char *key) { return data[key].size(); }
  size_t getBytes(const char *key, void *out, size_t length) {
    if (data[key].size() != length) return 0;
    memcpy(out, data[key].data(), length); return length;
  }
  size_t putBytes(const char *key, const void *value, size_t length) {
    if (failWrite) return 0;
    auto bytes = static_cast<const unsigned char *>(value);
    data[key] = std::vector<unsigned char>(bytes, bytes + length); return length;
  }
  bool remove(const char *key) { if (failWrite) return false; data.erase(key); return true; }
};
