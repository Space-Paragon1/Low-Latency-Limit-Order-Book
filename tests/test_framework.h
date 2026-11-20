#pragma once
// Minimal, dependency-free test framework.
//
//   TEST(name) { ASSERT_EQ(a, b); ... }
//
// Run all tests:            lob_tests
// Run tests matching text:  lob_tests replace stop     (substring match, any of)
// List tests:               lob_tests --list

#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace lobtest {

struct TestCase {
    const char* name;
    const char* file;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(const char* name, const char* file, void (*fn)()) {
        registry().push_back({name, file, fn});
    }
};

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

namespace detail {
// Streams `v` if it has an operator<<, otherwise prints a placeholder.
template <typename T>
auto tryStream(std::ostream& os, const T& v, int) -> decltype(os << v, void()) {
    os << v;
}
template <typename T>
void tryStream(std::ostream& os, const T&, long) {
    os << "<unprintable>";
}

template <typename A, typename B>
[[noreturn]] void failBinary(const char* op, const char* ea, const char* eb, const A& a,
                             const B& b, const char* file, int line) {
    std::ostringstream os;
    os << file << ':' << line << ": expected " << ea << ' ' << op << ' ' << eb
       << "\n           lhs = ";
    tryStream(os, a, 0);
    os << "\n           rhs = ";
    tryStream(os, b, 0);
    throw Failure(os.str());
}
}  // namespace detail

inline int runAll(int argc, char** argv) {
    std::vector<std::string_view> filters;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--list") {
            for (const auto& tc : registry()) std::cout << tc.name << '\n';
            return 0;
        }
        filters.push_back(arg);
    }
    auto selected = [&](const TestCase& tc) {
        if (filters.empty()) return true;
        for (auto f : filters)
            if (std::string_view(tc.name).find(f) != std::string_view::npos) return true;
        return false;
    };

    int pass = 0;
    std::vector<std::string> failed;
    const auto suite_start = std::chrono::steady_clock::now();
    for (const auto& tc : registry()) {
        if (!selected(tc)) continue;
        const auto t0 = std::chrono::steady_clock::now();
        try {
            tc.fn();
            const auto ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - t0).count();
            std::cout << "[  OK  ] " << tc.name << "  (" << ms << " ms)\n";
            ++pass;
        } catch (const std::exception& e) {
            std::cout << "[ FAIL ] " << tc.name << "\n         " << e.what() << '\n';
            failed.emplace_back(tc.name);
        }
    }
    const auto total_ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - suite_start).count();

    const auto total = static_cast<std::size_t>(pass) + failed.size();
    std::cout << "\n" << pass << " / " << total << " tests passed  (" << total_ms << " ms)\n";
    if (!failed.empty()) {
        std::cout << "Failed:\n";
        for (const auto& name : failed) std::cout << "  " << name << '\n';
        return 1;
    }
    return total == 0 ? 1 : 0;   // a filter that matches nothing is an error
}

}  // namespace lobtest

#define TEST(name)                                                                  \
    static void lobtest_fn_##name();                                                \
    static const ::lobtest::Registrar lobtest_reg_##name(#name, __FILE__,           \
                                                         &lobtest_fn_##name);       \
    static void lobtest_fn_##name()

#define LOBTEST_BINARY(op, a, b)                                                    \
    do {                                                                            \
        /* copies, not references: `a` may be a member of a temporary */          \
        const auto lobtest_a = (a);                                                 \
        const auto lobtest_b = (b);                                                 \
        if (!(lobtest_a op lobtest_b))                                              \
            ::lobtest::detail::failBinary(#op, #a, #b, lobtest_a, lobtest_b,        \
                                          __FILE__, __LINE__);                      \
    } while (0)

#define ASSERT_EQ(a, b) LOBTEST_BINARY(==, a, b)
#define ASSERT_NE(a, b) LOBTEST_BINARY(!=, a, b)
#define ASSERT_LT(a, b) LOBTEST_BINARY(<, a, b)
#define ASSERT_LE(a, b) LOBTEST_BINARY(<=, a, b)
#define ASSERT_GT(a, b) LOBTEST_BINARY(>, a, b)
#define ASSERT_GE(a, b) LOBTEST_BINARY(>=, a, b)

#define ASSERT_TRUE(x)                                                              \
    do {                                                                            \
        if (!(x)) {                                                                 \
            std::ostringstream lobtest_os;                                          \
            lobtest_os << __FILE__ << ':' << __LINE__ << ": expected " #x;          \
            throw ::lobtest::Failure(lobtest_os.str());                             \
        }                                                                           \
    } while (0)

#define ASSERT_FALSE(x) ASSERT_TRUE(!(x))

#define FAIL(msg)                                                                   \
    do {                                                                            \
        std::ostringstream lobtest_os;                                              \
        lobtest_os << __FILE__ << ':' << __LINE__ << ": " << msg;                   \
        throw ::lobtest::Failure(lobtest_os.str());                                 \
    } while (0)
