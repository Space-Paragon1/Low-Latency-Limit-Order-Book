#pragma once
#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <stdexcept>
#include <sstream>

// ─── Lightweight zero-dependency test framework ────────────────────────────────
struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

#define TEST(name)                                                  \
    static void _test_fn_##name();                                  \
    static Registrar _reg_##name(#name, _test_fn_##name);          \
    static void _test_fn_##name()

// ─── Stream helper ─────────────────────────────────────────────────────────────
// Falls back to "<unprintable>" for types that have no operator<<.
namespace detail {
    template<typename T>
    auto try_stream(std::ostream& os, const T& v, int)
        -> decltype(os << v, void()) { os << v; }
    template<typename T>
    void try_stream(std::ostream& os, const T&, ...) { os << "<unprintable>"; }
}

// ─── Assertion macros ─────────────────────────────────────────────────────────
#define ASSERT_EQ(a, b)                                                        \
    do {                                                                       \
        auto _a = (a); auto _b = (b);                                          \
        if (!(_a == _b)) {                                                     \
            std::ostringstream _oss;                                           \
            _oss << "ASSERT_EQ failed at " __FILE__ ":" << __LINE__            \
                 << "\n  lhs (" #a "): "; detail::try_stream(_oss, _a, 0);    \
            _oss << "\n  rhs (" #b "): "; detail::try_stream(_oss, _b, 0);    \
            throw std::runtime_error(_oss.str());                              \
        }                                                                      \
    } while (0)

#define ASSERT_NE(a, b)                                                        \
    do {                                                                       \
        auto _a = (a); auto _b = (b);                                          \
        if (_a == _b) {                                                        \
            std::ostringstream _oss;                                           \
            _oss << "ASSERT_NE failed at " __FILE__ ":" << __LINE__            \
                 << "\n  both equal (" #a " == " #b ")";                       \
            throw std::runtime_error(_oss.str());                              \
        }                                                                      \
    } while (0)

#define ASSERT_TRUE(x)                                                         \
    do {                                                                       \
        if (!(x)) {                                                            \
            std::ostringstream _oss;                                           \
            _oss << "ASSERT_TRUE failed at " __FILE__ ":" << __LINE__          \
                 << "\n  expression: " #x;                                     \
            throw std::runtime_error(_oss.str());                              \
        }                                                                      \
    } while (0)

#define ASSERT_FALSE(x) ASSERT_TRUE(!(x))

// ─── Runner ───────────────────────────────────────────────────────────────────
inline int run_all_tests() {
    int pass = 0, fail = 0;
    for (auto& tc : registry()) {
        std::cout << "[ RUN  ] " << tc.name << '\n';
        try {
            tc.fn();
            std::cout << "[ OK   ] " << tc.name << '\n';
            ++pass;
        } catch (const std::exception& e) {
            std::cout << "[ FAIL ] " << tc.name << '\n'
                      << "         " << e.what() << '\n';
            ++fail;
        }
    }
    std::cout << "\n─────────────────────────────────────────\n"
              << pass << " / " << (pass + fail) << " tests passed\n";
    return (fail > 0) ? 1 : 0;
}
