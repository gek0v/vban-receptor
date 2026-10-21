#pragma once

#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <sstream>
#include <cmath>
#include <chrono>
#include <exception>
#include <iomanip>

namespace vban::test {

struct AssertionFailure {
    std::string file;
    int line;
    std::string expression;
    std::string message;
};

class TestAbortException : public std::exception {
public:
    explicit TestAbortException(AssertionFailure failure) : m_failure(std::move(failure)) {}
    const AssertionFailure& failure() const noexcept { return m_failure; }
    const char* what() const noexcept override { return m_failure.message.c_str(); }
private:
    AssertionFailure m_failure;
};

struct TestContext {
    bool failed = false;
    std::vector<AssertionFailure> failures;
};

inline TestContext*& active_context() {
    static thread_local TestContext* ctx = nullptr;
    return ctx;
}

using TestFn = void(*)();

struct TestCase {
    std::string name;
    std::string suite;
    std::string file;
    int line;
    TestFn fn;
};

class TestRegistry {
public:
    static TestRegistry& instance() {
        static TestRegistry reg;
        return reg;
    }
    void register_test(std::string name, std::string suite, std::string file, int line, TestFn fn) {
        m_tests.push_back(TestCase{std::move(name), std::move(suite), std::move(file), line, fn});
    }
    const std::vector<TestCase>& tests() const { return m_tests; }
private:
    std::vector<TestCase> m_tests;
};

struct AutoRegister {
    AutoRegister(std::string name, std::string suite, std::string file, int line, TestFn fn) {
        TestRegistry::instance().register_test(std::move(name), std::move(suite), std::move(file), line, fn);
    }
};

template <typename T>
std::string format_val(const T& val) {
    if constexpr (std::is_enum_v<T>) {
        return std::to_string(static_cast<std::underlying_type_t<T>>(val));
    } else {
        std::ostringstream oss;
        oss << val;
        return oss.str();
    }
}

inline std::string format_val(uint8_t val) {
    std::ostringstream oss;
    oss << static_cast<int>(val) << " (0x" << std::hex << static_cast<int>(val) << std::dec << ")";
    return oss.str();
}

inline std::string format_val(const char* val) {
    return val ? std::string(val) : std::string("(null)");
}

inline std::string format_val(std::nullptr_t) {
    return "nullptr";
}

inline void record_failure(const char* file, int line, const char* expr, const std::string& msg, bool fatal) {
    AssertionFailure fail{file, line, expr, msg};
    if (active_context()) {
        active_context()->failed = true;
        active_context()->failures.push_back(fail);
    }
    if (fatal) {
        throw TestAbortException(std::move(fail));
    }
}

} // namespace vban::test

#define VBAN_TEST_CONCAT_IMPL(a, b) a##b
#define VBAN_TEST_CONCAT(a, b) VBAN_TEST_CONCAT_IMPL(a, b)

#define TEST_CASE(name) \
    static void VBAN_TEST_CONCAT(_vban_test_fn_, __LINE__)(); \
    static const ::vban::test::AutoRegister VBAN_TEST_CONCAT(_vban_reg_, __LINE__)( \
        name, __FILE__, __FILE__, __LINE__, &VBAN_TEST_CONCAT(_vban_test_fn_, __LINE__)); \
    static void VBAN_TEST_CONCAT(_vban_test_fn_, __LINE__)()

// Fatal Assertions (Aborts current test on failure)
#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        ::vban::test::record_failure(__FILE__, __LINE__, #cond, "Expected true, evaluated to false", true); \
    } \
} while(0)

#define ASSERT_FALSE(cond) do { \
    if (cond) { \
        ::vban::test::record_failure(__FILE__, __LINE__, #cond, "Expected false, evaluated to true", true); \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    auto _va = (a); auto _vb = (b); \
    if (!(_va == _vb)) { \
        std::ostringstream _oss; \
        _oss << "Expected equal: " << ::vban::test::format_val(_va) << " == " << ::vban::test::format_val(_vb); \
        ::vban::test::record_failure(__FILE__, __LINE__, #a " == " #b, _oss.str(), true); \
    } \
} while(0)

#define ASSERT_NE(a, b) do { \
    auto _va = (a); auto _vb = (b); \
    if (_va == _vb) { \
        std::ostringstream _oss; \
        _oss << "Expected not equal: both values are " << ::vban::test::format_val(_va); \
        ::vban::test::record_failure(__FILE__, __LINE__, #a " != " #b, _oss.str(), true); \
    } \
} while(0)

#define ASSERT_NEAR(a, b, eps) do { \
    auto _va = (a); auto _vb = (b); auto _eps = (eps); \
    auto _diff = std::abs(_va - _vb); \
    if (_diff > _eps) { \
        std::ostringstream _oss; \
        _oss << "Expected near: |" << _va << " - " << _vb << "| = " << _diff << " > epsilon (" << _eps << ")"; \
        ::vban::test::record_failure(__FILE__, __LINE__, "|" #a " - " #b "| <= " #eps, _oss.str(), true); \
    } \
} while(0)

#define ASSERT_LE(a, b) do { \
    auto _va = (a); auto _vb = (b); \
    if (!(_va <= _vb)) { \
        std::ostringstream _oss; _oss << "Expected: " << _va << " <= " << _vb; \
        ::vban::test::record_failure(__FILE__, __LINE__, #a " <= " #b, _oss.str(), true); \
    } \
} while(0)

#define ASSERT_GE(a, b) do { \
    auto _va = (a); auto _vb = (b); \
    if (!(_va >= _vb)) { \
        std::ostringstream _oss; _oss << "Expected: " << _va << " >= " << _vb; \
        ::vban::test::record_failure(__FILE__, __LINE__, #a " >= " #b, _oss.str(), true); \
    } \
} while(0)

#define ASSERT_LT(a, b) do { \
    auto _va = (a); auto _vb = (b); \
    if (!(_va < _vb)) { \
        std::ostringstream _oss; _oss << "Expected: " << _va << " < " << _vb; \
        ::vban::test::record_failure(__FILE__, __LINE__, #a " < " #b, _oss.str(), true); \
    } \
} while(0)

#define ASSERT_GT(a, b) do { \
    auto _va = (a); auto _vb = (b); \
    if (!(_va > _vb)) { \
        std::ostringstream _oss; _oss << "Expected: " << _va << " > " << _vb; \
        ::vban::test::record_failure(__FILE__, __LINE__, #a " > " #b, _oss.str(), true); \
    } \
} while(0)

// Non-fatal Expectations (Continues execution of test)
#define EXPECT_TRUE(cond) do { \
    if (!(cond)) { \
        ::vban::test::record_failure(__FILE__, __LINE__, #cond, "Expected true, evaluated to false", false); \
    } \
} while(0)

#define EXPECT_FALSE(cond) do { \
    if (cond) { \
        ::vban::test::record_failure(__FILE__, __LINE__, #cond, "Expected false, evaluated to true", false); \
    } \
} while(0)

#define EXPECT_EQ(a, b) do { \
    auto _va = (a); auto _vb = (b); \
    if (!(_va == _vb)) { \
        std::ostringstream _oss; \
        _oss << "Expected equal: " << ::vban::test::format_val(_va) << " == " << ::vban::test::format_val(_vb); \
        ::vban::test::record_failure(__FILE__, __LINE__, #a " == " #b, _oss.str(), false); \
    } \
} while(0)

#define EXPECT_NEAR(a, b, eps) do { \
    auto _va = (a); auto _vb = (b); auto _eps = (eps); \
    auto _diff = std::abs(_va - _vb); \
    if (_diff > _eps) { \
        std::ostringstream _oss; \
        _oss << "Expected near: |" << _va << " - " << _vb << "| = " << _diff << " > epsilon (" << _eps << ")"; \
        ::vban::test::record_failure(__FILE__, __LINE__, "|" #a " - " #b "| <= " #eps, _oss.str(), false); \
    } \
} while(0)
