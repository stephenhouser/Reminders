// Minimal test harness: TEST(name) { CHECK(...); CHECK_EQ(a, b); }
#pragma once

#include <format>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace test {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failures() {
    static int n = 0;
    return n;
}

struct Register {
    Register(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

template <class T>
std::string show(const T& v) {
    if constexpr (std::formattable<T, char>) return std::format("{}", v);
    else return "<value>";
}

}  // namespace test

#define TEST_CONCAT2(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT2(a, b)
#define TEST(name)                                                                  \
    static void name();                                                             \
    static test::Register TEST_CONCAT(reg_, name){#name, name};                     \
    static void name()

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            ++test::failures();                                                     \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK(" #cond ") failed\n"; \
        }                                                                           \
    } while (0)

#define CHECK_EQ(a, b)                                                              \
    do {                                                                            \
        auto&& va_ = (a);                                                           \
        auto&& vb_ = (b);                                                           \
        if (!(va_ == vb_)) {                                                        \
            ++test::failures();                                                     \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK_EQ(" #a ", " #b ") failed\n" \
                      << "  left:  " << test::show(va_) << "\n"                     \
                      << "  right: " << test::show(vb_) << "\n";                    \
        }                                                                           \
    } while (0)
