#pragma once
#include <cstdint>
#include <cstring>
#include <cctype>
#include <string>
class String : public std::string {
 public:
  using std::string::string;
  using std::string::operator=;
  int indexOf(const char *value) const { auto p = find(value); return p == npos ? -1 : static_cast<int>(p); }
};
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
