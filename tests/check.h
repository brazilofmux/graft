// check.h: a deliberately tiny test harness (no external test framework).
//
//   TEST(name) { CHECK(expr); CHECK_EQ(a, b); }
//
// Tests self-register; test_main.cpp runs them all, or those whose names
// contain argv[1].
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace check {

struct Case {
    const char *name;
    void (*fn)();
};

inline std::vector<Case> &registry() {
    static std::vector<Case> r;
    return r;
}

inline int &failures() {
    static int n = 0;
    return n;
}

struct Registrar {
    Registrar(const char *name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void fail(const char *file, int line, const std::string &what) {
    std::fprintf(stderr, "  %s:%d: %s\n", file, line, what.c_str());
    ++failures();
}

}  // namespace check

#define CHECK_CAT2(a, b) a##b
#define CHECK_CAT(a, b) CHECK_CAT2(a, b)

#define TEST(name)                                                               \
    static void CHECK_CAT(test_, name)();                                        \
    static const ::check::Registrar CHECK_CAT(reg_, name){#name, &CHECK_CAT(test_, name)}; \
    static void CHECK_CAT(test_, name)()

#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) ::check::fail(__FILE__, __LINE__, "CHECK(" #expr ")"); \
    } while (0)

#define CHECK_EQ(a, b)                                                          \
    do {                                                                        \
        if (!((a) == (b))) ::check::fail(__FILE__, __LINE__, "CHECK_EQ(" #a ", " #b ")"); \
    } while (0)
