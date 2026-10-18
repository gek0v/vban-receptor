#include "test_framework.hpp"

#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <span>
#include <concepts>
#include <bit>
#include <atomic>
#include <thread>
#include <memory>

#ifdef _WIN32
#include <windows.h>
static void enable_windows_ansi() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD dwMode = 0;
        if (GetConsoleMode(hOut, &dwMode)) {
            dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hOut, dwMode);
        }
    }
}
#else
static void enable_windows_ansi() {}
#endif

// ==============================================================================
// 1. Test Framework Sanity Tests
// ==============================================================================
TEST_CASE("framework_basic_assertions") {
    ASSERT_TRUE(true);
    ASSERT_FALSE(false);
    ASSERT_EQ(42, 42);
    ASSERT_NE(42, 43);
    ASSERT_LE(10, 20);
    ASSERT_LE(20, 20);
    ASSERT_GE(30, 20);
    ASSERT_GE(30, 30);
    ASSERT_LT(5, 10);
    ASSERT_GT(10, 5);
    ASSERT_NEAR(1.0001, 1.0002, 0.001);
    EXPECT_TRUE(1 < 2);
    EXPECT_FALSE(2 < 1);
    EXPECT_EQ(100, 100);
    EXPECT_NEAR(3.14159f, 3.14150f, 0.001f);
}

// ==============================================================================
// 2. C++20 Feature Conformance Tests
// ==============================================================================
template <typename T>
concept SampleConcept = std::floating_point<T> || std::integral<T>;

TEST_CASE("cpp20_concepts_and_endianness") {
    static_assert(SampleConcept<float>);
    static_assert(SampleConcept<double>);
    static_assert(SampleConcept<int16_t>);
    static_assert(SampleConcept<int32_t>);
    static_assert(!SampleConcept<std::string>);

    constexpr bool is_little = (std::endian::native == std::endian::little);
    ASSERT_TRUE(is_little); // Host x86_64 and Android ARM64 are both Little Endian
}

TEST_CASE("cpp20_span_operations") {
    std::vector<float> data = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f};
    std::span<float> span_view(data);
    ASSERT_EQ(span_view.size(), 5U);
    ASSERT_NEAR(span_view[0], 0.1f, 1e-6f);
    ASSERT_NEAR(span_view[4], 0.5f, 1e-6f);

    auto sub = span_view.subspan(1, 3);
    ASSERT_EQ(sub.size(), 3U);
    ASSERT_NEAR(sub[0], 0.2f, 1e-6f);
    ASSERT_NEAR(sub[2], 0.4f, 1e-6f);
}

// ==============================================================================
// Main Test Runner Entrypoint
// ==============================================================================
int main(int argc, char** argv) {
    enable_windows_ansi();

    std::string filter;
    bool list_only = false;
    bool no_color = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--list" || arg == "-l") {
            list_only = true;
        } else if (arg.rfind("--filter=", 0) == 0) {
            filter = arg.substr(9);
        } else if (arg == "-f" && i + 1 < argc) {
            filter = argv[++i];
        } else if (arg == "--no-color") {
            no_color = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: vban_native_tests [options]\n"
                      << "Options:\n"
                      << "  -l, --list            List all registered tests\n"
                      << "  -f, --filter=<substr> Run tests matching substring\n"
                      << "  --no-color            Disable ANSI color escape codes\n"
                      << "  -h, --help            Show this help message\n";
            return 0;
        }
    }

    const auto& tests = vban::test::TestRegistry::instance().tests();

    if (list_only) {
        std::cout << "Registered tests (" << tests.size() << "):\n";
        for (size_t i = 0; i < tests.size(); ++i) {
            std::cout << "  [" << i << "] " << tests[i].name << " (" << tests[i].file << ":" << tests[i].line << ")\n";
        }
        return 0;
    }

    std::string clr_green = no_color ? "" : "\033[32m";
    std::string clr_red   = no_color ? "" : "\033[31m";
    std::string clr_bold  = no_color ? "" : "\033[1m";
    std::string clr_reset = no_color ? "" : "\033[0m";

    std::cout << clr_bold << "=== VBAN Native C++20 Test Runner ===" << clr_reset << "\n";
    if (!filter.empty()) {
        std::cout << "Filter active: \"" << filter << "\"\n";
    }

    int run_count = 0;
    int pass_count = 0;
    int fail_count = 0;

    auto suite_start = std::chrono::high_resolution_clock::now();

    for (const auto& tc : tests) {
        if (!filter.empty() && tc.name.find(filter) == std::string::npos) {
            continue;
        }

        run_count++;
        vban::test::TestContext ctx;
        vban::test::active_context() = &ctx;

        auto test_start = std::chrono::high_resolution_clock::now();
        try {
            tc.fn();
        } catch (const vban::test::TestAbortException&) {
            // Failure recorded in ctx
        } catch (const std::exception& e) {
            ctx.failed = true;
            ctx.failures.push_back({__FILE__, __LINE__, "std::exception", e.what()});
        } catch (...) {
            ctx.failed = true;
            ctx.failures.push_back({__FILE__, __LINE__, "unknown", "Unknown exception thrown"});
        }
        auto test_end = std::chrono::high_resolution_clock::now();
        double elapsed_ms = std::chrono::duration<double, std::milli>(test_end - test_start).count();

        vban::test::active_context() = nullptr;

        if (!ctx.failed) {
            pass_count++;
            std::cout << "  " << clr_green << "[PASS]" << clr_reset << " " << tc.name
                      << " (" << std::fixed << std::setprecision(3) << elapsed_ms << " ms)\n";
        } else {
            fail_count++;
            std::cout << "  " << clr_red << "[FAIL]" << clr_reset << " " << tc.name
                      << " (" << std::fixed << std::setprecision(3) << elapsed_ms << " ms)\n";
            for (const auto& f : ctx.failures) {
                std::cout << "         " << f.file << ":" << f.line << ": Assertion failed: " << f.expression << "\n";
                std::cout << "         Details: " << f.message << "\n";
            }
        }
    }

    auto suite_end = std::chrono::high_resolution_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(suite_end - suite_start).count();

    std::cout << clr_bold << "--------------------------------------------------------" << clr_reset << "\n";
    std::cout << "Summary: "
              << (fail_count == 0 ? clr_green : clr_red)
              << pass_count << "/" << run_count << " passed" << clr_reset
              << ", " << fail_count << " failed"
              << " (" << total_ms << " ms total)\n";

    return (fail_count == 0) ? 0 : 1;
}
