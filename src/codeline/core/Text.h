// Small, allocation-free text helpers shared by generated tables and codecs.
#pragma once

#include <charconv>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace codeline {

// Index of `s` in `names[0..count)`, or -1.
inline int lookupToken(const char* const* names, uint8_t count, std::string_view s) {
  for (uint8_t i = 0; i < count; ++i) {
    if (names[i] != nullptr && s == names[i]) return i;
  }
  return -1;
}

// Logical names of a generated enum; specialized in codeline/generated/Enums.h.
template <class E>
struct EnumText;

template <class E>
const char* nameOf(E v) {
  return EnumText<E>::names[static_cast<uint8_t>(v)];
}

template <class E>
bool parseName(std::string_view s, E& out) {
  const int i = lookupToken(EnumText<E>::names, EnumText<E>::count, s);
  if (i < 0) return false;
  out = static_cast<E>(i);
  return true;
}

// Strict base-10 unsigned parse: the whole of `s`, no sign, no spaces, <= max.
inline bool parseUint(std::string_view s, uint32_t max, uint32_t& out) {
  if (s.empty()) return false;
  uint64_t v = 0;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc() || r.ptr != s.data() + s.size() || v > max) return false;
  out = static_cast<uint32_t>(v);
  return true;
}

inline std::string_view trim(std::string_view s) {
  while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
  return s;
}

// Splits on single or repeated spaces.
class Tokens {
  std::string_view rest_;

public:
  explicit Tokens(std::string_view s) : rest_(s) {}
  bool next(std::string_view& out) {
    rest_ = trim(rest_);
    if (rest_.empty()) return false;
    const size_t end = rest_.find(' ');
    out = rest_.substr(0, end);
    rest_ = end == std::string_view::npos ? std::string_view() : rest_.substr(end);
    return true;
  }
  std::string_view rest() const { return trim(rest_); }
};

// Bounded string storage. Appends past capacity are dropped and ok() turns false.
template <size_t N>
class FixedString {
  char buf_[N + 1] = {};
  size_t len_ = 0;
  bool ok_ = true;

public:
  FixedString& append(std::string_view s) {
    if (len_ + s.size() > N) {
      ok_ = false;
      return *this;
    }
    std::memcpy(buf_ + len_, s.data(), s.size());
    len_ += s.size();
    buf_[len_] = '\0';
    return *this;
  }
  FixedString& append(uint32_t n) {
    char digits[11];
    const auto r = std::to_chars(digits, digits + sizeof digits, n);
    return append(std::string_view(digits, static_cast<size_t>(r.ptr - digits)));
  }
  void clear() {
    len_ = 0;
    ok_ = true;
    buf_[0] = '\0';
  }
  bool ok() const { return ok_; }
  std::string_view view() const { return std::string_view(buf_, len_); }
  const char* c_str() const { return buf_; }
};

}  // namespace codeline
