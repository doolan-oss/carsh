// Minimal host-side unit-test harness for carsh.
//
// Header-only, no external deps, so it links into any host test binary. Tests
// register themselves at static-init time; main() (in Test.cpp) runs them all
// and reports pass/fail counts. Assertions are real: a failed CHECK/REQUIRE
// records the file:line and the stringized expression. REQUIRE aborts the
// current test; CHECK continues so several failures surface from one run.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <functional>

namespace test {

struct Case {
    const char* suite;
    const char* name;
    std::function<void()> fn;
};

// Thrown by REQUIRE to unwind out of a failing test without aborting the process.
struct AbortCase {};

class Registry {
public:
    static Registry& instance() {
        static Registry r;
        return r;
    }
    void add(const Case& c) { cases_.push_back(c); }

    int run(const char* filter) {
        int passed = 0, failed = 0, skipped = 0;
        for (auto& c : cases_) {
            if (filter && *filter && !std::strstr(c.name, filter) &&
                !std::strstr(c.suite, filter)) {
                ++skipped;
                continue;
            }
            curFailures_ = 0;
            std::printf("[ RUN  ] %s.%s\n", c.suite, c.name);
            try {
                c.fn();
            } catch (const AbortCase&) {
                // REQUIRE already logged the reason.
            } catch (const std::exception& e) {
                fail(__FILE__, __LINE__, "uncaught std::exception");
                std::printf("         what(): %s\n", e.what());
            } catch (...) {
                fail(__FILE__, __LINE__, "uncaught unknown exception");
            }
            if (curFailures_ == 0) {
                std::printf("[ PASS ] %s.%s\n", c.suite, c.name);
                ++passed;
            } else {
                std::printf("[ FAIL ] %s.%s (%d failure%s)\n", c.suite, c.name,
                            curFailures_, curFailures_ == 1 ? "" : "s");
                ++failed;
            }
        }
        std::printf("\n=== %d passed, %d failed, %d skipped ===\n", passed,
                    failed, skipped);
        return failed == 0 ? 0 : 1;
    }

    void fail(const char* file, int line, const char* expr) {
        std::printf("  [FAILURE] %s:%d: %s\n", file, line, expr);
        ++curFailures_;
    }
    int curFailures() const { return curFailures_; }

private:
    std::vector<Case> cases_;
    int curFailures_ = 0;
};

struct Registrar {
    Registrar(const char* suite, const char* name, std::function<void()> fn) {
        Registry::instance().add({suite, name, std::move(fn)});
    }
};

}  // namespace test

#define TEST_CONCAT_(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT_(a, b)

#define TEST(suite, name)                                                    \
    static void TEST_CONCAT(test_fn_, __LINE__)();                           \
    static ::test::Registrar TEST_CONCAT(test_reg_, __LINE__)(               \
        #suite, #name, &TEST_CONCAT(test_fn_, __LINE__));                    \
    static void TEST_CONCAT(test_fn_, __LINE__)()

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond))                                                         \
            ::test::Registry::instance().fail(__FILE__, __LINE__, #cond);    \
    } while (0)

#define REQUIRE(cond)                                                        \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ::test::Registry::instance().fail(__FILE__, __LINE__, #cond);    \
            throw ::test::AbortCase{};                                       \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        auto va_ = (a);                                                       \
        auto vb_ = (b);                                                       \
        if (!(va_ == vb_))                                                    \
            ::test::Registry::instance().fail(__FILE__, __LINE__,             \
                                              #a " == " #b);                  \
    } while (0)

// Floating-point near-equality with an absolute tolerance.
#define CHECK_NEAR(a, b, tol)                                                 \
    do {                                                                      \
        double va_ = (double)(a);                                            \
        double vb_ = (double)(b);                                            \
        if (std::fabs(va_ - vb_) > (tol)) {                                  \
            ::test::Registry::instance().fail(__FILE__, __LINE__,            \
                                              #a " ~= " #b);                 \
            std::printf("           %.9g vs %.9g (tol %.3g)\n", va_, vb_,    \
                        (double)(tol));                                      \
        }                                                                    \
    } while (0)
