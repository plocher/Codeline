// Minimal support for functional scenarios. No framework: each scenarios/*.cpp
// is a program with its own main() that returns non-zero if any check failed.
//
//   SCENARIO("a timer survives clock rollover") {
//     ...
//     CHECK(timer == 200);
//   }
//   int main() { return codeline::scenario::run(); }
#pragma once

#include <cstdio>
#include <vector>

namespace codeline::scenario {

struct Case {
  const char* name;
  void (*body)();
};

inline std::vector<Case>& cases() {
  static std::vector<Case> all;
  return all;
}

inline int& failures() {
  static int count = 0;
  return count;
}

struct Registrar {
  Registrar(const char* name, void (*body)()) { cases().push_back({name, body}); }
};

inline int run() {
  for (const Case& c : cases()) {
    const int before = failures();
    c.body();
    std::printf("  %s  %s\n", failures() == before ? "ok  " : "FAIL", c.name);
  }
  return failures() == 0 ? 0 : 1;
}

}  // namespace codeline::scenario

#define SCENARIO_CAT2(a, b) a##b
#define SCENARIO_CAT(a, b) SCENARIO_CAT2(a, b)

#define SCENARIO(name)                                                  \
  static void SCENARIO_CAT(scenario_body_, __LINE__)();                 \
  static codeline::scenario::Registrar SCENARIO_CAT(scenario_reg_, __LINE__)( \
      name, SCENARIO_CAT(scenario_body_, __LINE__));                    \
  static void SCENARIO_CAT(scenario_body_, __LINE__)()

#define CHECK(expr)                                                     \
  do {                                                                  \
    if (!(expr)) {                                                      \
      ++codeline::scenario::failures();                                 \
      std::printf("    %s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
    }                                                                   \
  } while (0)
