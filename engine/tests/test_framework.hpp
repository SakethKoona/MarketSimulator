#pragma once
// Minimal test harness: no dependencies, one binary, non-zero exit on failure.
//
//   TEST(name) { ... }            registers a test
//   EXPECT(cond)                  records a failure and continues
//   EXPECT_EQ(a, b)               same, prints both values
//   REQUIRE(cond)                 records a failure and aborts the test

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase> &registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char *name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

struct RequireFailed {};

inline int &failures() {
    static int n = 0;
    return n;
}

inline void fail(const char *file, int line, const std::string &msg) {
    failures()++;
    std::cerr << "    FAIL " << file << ":" << line << "  " << msg << "\n";
}

template <typename A, typename B>
std::string describe(const char *ea, const char *eb, const A &a, const B &b) {
    std::ostringstream ss;
    ss << ea << " == " << eb << "  (" << a << " vs " << b << ")";
    return ss.str();
}

inline int run_all(int argc, char **argv) {
    std::string filter = argc > 1 ? argv[1] : "";
    int ran = 0, failed = 0;
    for (auto &t : registry()) {
        if (!filter.empty() && t.name.find(filter) == std::string::npos)
            continue;
        int before = failures();
        std::cout << "[ RUN  ] " << t.name << "\n";
        try {
            t.fn();
        } catch (const RequireFailed &) {
        } catch (const std::exception &e) {
            fail("<exception>", 0, e.what());
        }
        ran++;
        if (failures() == before) {
            std::cout << "[  OK  ] " << t.name << "\n";
        } else {
            failed++;
            std::cout << "[ FAIL ] " << t.name << "\n";
        }
    }
    std::cout << "\n" << ran << " tests, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}

} // namespace testing

#define TEST(name)                                                             \
    static void test_##name();                                                 \
    static testing::Registrar registrar_##name(#name, test_##name);            \
    static void test_##name()

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond))                                                           \
            testing::fail(__FILE__, __LINE__, #cond);                          \
    } while (0)

#define EXPECT_EQ(a, b)                                                        \
    do {                                                                       \
        auto va_ = (a);                                                        \
        auto vb_ = (b);                                                        \
        if (!(va_ == vb_))                                                     \
            testing::fail(__FILE__, __LINE__, testing::describe(#a, #b, va_, vb_)); \
    } while (0)

#define REQUIRE(cond)                                                          \
    do {                                                                       \
        if (!(cond)) {                                                         \
            testing::fail(__FILE__, __LINE__, #cond);                          \
            throw testing::RequireFailed{};                                    \
        }                                                                      \
    } while (0)
