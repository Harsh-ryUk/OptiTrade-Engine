#pragma once

// Minimal self-registering test harness (no external dependencies).
//
//   #include "check.hpp"
//   OT_TEST(adds_up) { OT_CHECK_EQ(1 + 1, 2); }
//   OT_TEST_MAIN()
//
// A failed check prints file:line and marks the test failed, but the test keeps
// running so one run reports every problem.

#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ot_test {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}
inline int& failures_in_current() {
    static int n = 0;
    return n;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

template <class T>
std::string show(const T& v) {
    if constexpr (std::is_enum_v<T>) {
        return std::to_string(static_cast<long long>(v));
    } else if constexpr (std::is_same_v<T, bool>) {
        return v ? "true" : "false";
    } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
        return std::to_string(static_cast<long long>(v));
    } else if constexpr (std::is_integral_v<T>) {
        return std::to_string(static_cast<unsigned long long>(v));
    } else if constexpr (std::is_convertible_v<T, std::string_view>) {
        return "\"" + std::string(std::string_view(v)) + "\"";
    } else {
        return "<value>";
    }
}

inline void fail(const char* file, int line, const std::string& msg) {
    std::fprintf(stderr, "  FAIL %s:%d  %s\n", file, line, msg.c_str());
    ++failures_in_current();
}

inline int run_all() {
    int failed_cases = 0;
    for (const Case& c : registry()) {
        failures_in_current() = 0;
        c.fn();
        if (failures_in_current() != 0) {
            std::fprintf(stderr, "[FAILED] %s (%d check(s))\n", c.name, failures_in_current());
            ++failed_cases;
        } else {
            std::printf("[ ok ] %s\n", c.name);
        }
    }
    std::printf("%zu test case(s), %d failed\n", registry().size(), failed_cases);
    return failed_cases == 0 ? 0 : 1;
}

}  // namespace ot_test

#define OT_TEST(name)                                                  \
    static void name();                                                \
    static ::ot_test::Registrar ot_registrar_##name{#name, &name};     \
    static void name()

#define OT_CHECK(cond)                                                            \
    do {                                                                          \
        if (!(cond)) ::ot_test::fail(__FILE__, __LINE__, "CHECK(" #cond ")");     \
    } while (0)

#define OT_CHECK_EQ(a, b)                                                                  \
    do {                                                                                   \
        const auto& ot_a = (a);                                                            \
        const auto& ot_b = (b);                                                            \
        if (!(ot_a == ot_b))                                                               \
            ::ot_test::fail(__FILE__, __LINE__,                                            \
                            std::string("CHECK_EQ(" #a ", " #b ")  got ") +                \
                                ::ot_test::show(ot_a) + " vs " + ::ot_test::show(ot_b));   \
    } while (0)

#define OT_TEST_MAIN() \
    int main() { return ::ot_test::run_all(); }
