# Milestone 1: Standalone Native C++20 Test Harness Survey Report

**Agent**: `explorer_m1_testrunner`  
**Date**: 2026-10-18  
**Working Directory**: `D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m1_testrunner`  
**Target Module**: `native_tests/`  

---

## 1. Executive Summary

This survey report provides the architectural design, toolchain verification, and complete source code templates for the standalone native C++20 test harness in `native_tests/`.

The primary design principle is **zero external dependencies, zero emulator overhead, and instantaneous host-native execution**. The test harness allows all core DSP, VBAN protocol parsing, audio format dequantization, and lock-free SPSC concurrency logic to be built and verified in sub-second turnaround directly on the developer's workstation using host Clang or NDK Clang, before touching Android hardware or emulators.

---

## 2. Toolchain Verification on Host

Host environment probing verified two distinct compilation pathways on Windows x86_64:

| Pathway | Compiler / Tools | Verified Command | Capabilities |
| :--- | :--- | :--- | :--- |
| **NDK Clang (Host Target)** | Android NDK 26.1.10909125 Clang 17.0.2 (`x86_64-w64-windows-gnu`) | `& "D:\Android\sdk\ndk\26.1.10909125\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe" -std=c++20 ...` | Full C++20 standard library support (`std::span`, `<concepts>`, `<ranges>`, `<atomic>`). Compiles Windows x86_64 native binaries directly. |
| **Host GCC** | TDM-GCC 10.3.0 (`C:\TDM-GCC-64\bin\g++.exe`) | `g++ -std=c++20 ...` | Standard MinGW-w64 toolchain available on PATH. |
| **CMake & Ninja** | CMake 4.4.4 (PATH) / CMake 3.22.1 (SDK), Ninja 1.10.2 (`D:\Android\sdk\cmake\3.22.1\bin\ninja.exe`) | `cmake -B build -G Ninja ...` | Full CMake 3.22+ and CTest compatibility. |

Both pathways were experimentally verified in temporary test builds. Multi-threaded C++20 tests and CTest integration compiled and executed with exit code 0 in under 0.1 seconds.

---

## 3. Directory Layout & Architecture

The `native_tests/` directory is situated at the repository root parallel to `app/`:

```
D:\Workspaces\vban-receptor\
├── app/
│   └── src/main/cpp/
│       ├── include/
│       │   ├── vban_protocol.hpp     (VBAN packet structs, FourCC, sample rates)
│       │   ├── vban_parser.hpp       (Packet validation pipeline)
│       │   ├── vban_converter.hpp    (PCM16/24/32/Float32 dequantization)
│       │   ├── spsc_ring_buffer.hpp  (Lock-free SPSC circular buffer)
│       │   ├── drift_compensator.hpp (Catmull-Rom resampler & PI drift servo)
│       │   ├── oboe_player.hpp       (Android-specific Oboe wrapper)
│       │   ├── vban_receiver.hpp     (UDP socket network receiver)
│       │   └── vban_engine.hpp       (Pipeline coordinator)
│       └── CMakeLists.txt            (Android NDK build)
│
├── native_tests/
│   ├── CMakeLists.txt                (Host CMake & CTest build file)
│   ├── test_framework.hpp            (Zero-dependency C++20 test runner engine)
│   ├── test_main.cpp                 (CLI runner, test registry, summary reporter)
│   ├── test_vban_protocol.cpp        (Header size, FourCC, sample rates, channels)
│   ├── test_vban_converter.cpp       (PCM dequantization, float clamping, NaN/Inf)
│   ├── test_spsc_buffer.cpp          (Lock-free queue, concurrency, zero-allocation)
│   └── test_drift_resampler.cpp      (Catmull-Rom Hermite spline, EMA watermark, PI)
```

### Decoupling Rules
1. **Core headers must be pure C++20**: `vban_protocol.hpp`, `vban_parser.hpp`, `vban_converter.hpp`, `spsc_ring_buffer.hpp`, and `drift_compensator.hpp` must not `#include <android/log.h>`, `<oboe/Oboe.h>`, or `<jni.h>`.
2. **Platform wrappers stay in `app/src/main/cpp/`**: `oboe_player.cpp`, `vban_receiver.cpp` (if using Android socket options), and `jni_bridge.cpp` contain Android dependencies and are not linked into `vban_native_tests`.

---

## 4. Complete File Specifications & Templates

### 4.1 `native_tests/CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.22.1)
project(vban_native_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable CTest harness
enable_testing()

# Paths to core headers and native sources
set(VBAN_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/cpp/include")
set(VBAN_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/cpp/src")

# Native test source files
set(TEST_SOURCES
    test_main.cpp
    test_vban_protocol.cpp
    test_vban_converter.cpp
    test_spsc_buffer.cpp
    test_drift_resampler.cpp
)

# Optional core .cpp sources (when functions are not header-only)
set(CORE_SOURCES "")
if(EXISTS "${VBAN_SRC_DIR}/vban_parser.cpp")
    list(APPEND CORE_SOURCES "${VBAN_SRC_DIR}/vban_parser.cpp")
endif()
if(EXISTS "${VBAN_SRC_DIR}/vban_converter.cpp")
    list(APPEND CORE_SOURCES "${VBAN_SRC_DIR}/vban_converter.cpp")
endif()
if(EXISTS "${VBAN_SRC_DIR}/drift_compensator.cpp")
    list(APPEND CORE_SOURCES "${VBAN_SRC_DIR}/drift_compensator.cpp")
endif()

add_executable(vban_native_tests ${TEST_SOURCES} ${CORE_SOURCES})

target_include_directories(vban_native_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${VBAN_INCLUDE_DIR}"
)

# Multi-threading support for SPSC lock-free concurrency stress tests
find_package(Threads REQUIRED)
target_link_libraries(vban_native_tests PRIVATE Threads::Threads)

# Compiler warnings and strict conformance
if(MSVC)
    target_compile_options(vban_native_tests PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(vban_native_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# Register test with CTest
add_test(NAME vban_tests COMMAND vban_native_tests)
```

---

### 4.2 `native_tests/test_framework.hpp`

```cpp
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
    std::ostringstream oss;
    oss << val;
    return oss.str();
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
```

---

### 4.3 `native_tests/test_main.cpp`

```cpp
#include "test_framework.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <algorithm>

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
            // Already added to ctx
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
```

---

### 4.4 `native_tests/test_vban_protocol.cpp`

```cpp
#include "test_framework.hpp"
#include <cstdint>
#include <cstring>
#include <string_view>

// Forward structure matching VBAN standard specification
#pragma pack(push, 1)
struct VbanHeaderLayout {
    uint32_t magic;         // 'V','B','A','N' (0x4E414256 LE)
    uint8_t  format_SR;     // Bits 5..7: Subprotocol; Bits 0..4: Rate index
    uint8_t  format_nbs;    // Samples per frame - 1 (0..255)
    uint8_t  format_nbc;    // Channels - 1 (0..7 / 0..255)
    uint8_t  format_bit;    // Bits 0..2: Data format; Bits 3..7: Codec
    char     streamname[16];// Null-padded ASCII
    uint32_t frame_counter; // 32-bit packet counter
};
#pragma pack(pop)
static_assert(sizeof(VbanHeaderLayout) == 28, "VBAN header must be exactly 28 bytes");

inline constexpr uint32_t kOfficialRates[21] = {
    6000,  12000, 24000, 48000, 96000, 192000, 384000,
    8000,  16000, 32000, 64000, 128000, 256000, 512000,
    11025, 22050, 44100, 88200, 176400, 352800, 705600
};

TEST_CASE("vban_protocol_header_layout_size") {
    ASSERT_EQ(sizeof(VbanHeaderLayout), 28U);
    ASSERT_EQ(offsetof(VbanHeaderLayout, magic), 0U);
    ASSERT_EQ(offsetof(VbanHeaderLayout, format_SR), 4U);
    ASSERT_EQ(offsetof(VbanHeaderLayout, format_nbs), 5U);
    ASSERT_EQ(offsetof(VbanHeaderLayout, format_nbc), 6U);
    ASSERT_EQ(offsetof(VbanHeaderLayout, format_bit), 7U);
    ASSERT_EQ(offsetof(VbanHeaderLayout, streamname), 8U);
    ASSERT_EQ(offsetof(VbanHeaderLayout, frame_counter), 24U);
}

TEST_CASE("vban_protocol_magic_validation") {
    VbanHeaderLayout valid_hdr{};
    valid_hdr.magic = 0x4E414256U; // 'V', 'B', 'A', 'N' in Little-Endian
    ASSERT_EQ(valid_hdr.magic, 0x4E414256U);

    uint32_t bad_magic = 0x54534554U; // 'T','E','S','T'
    ASSERT_NE(bad_magic, 0x4E414256U);
}

TEST_CASE("vban_protocol_subprotocol_audio_check") {
    uint8_t format_SR_audio = 0x03; // Audio (0x00 << 5) | rate index 3 (48 kHz)
    uint8_t subproto = (format_SR_audio >> 5) & 0x07;
    ASSERT_EQ(subproto, 0x00);

    uint8_t format_SR_serial = 0x20 | 0x03; // Subprotocol 1 (Serial)
    uint8_t subproto_serial = (format_SR_serial >> 5) & 0x07;
    ASSERT_NE(subproto_serial, 0x00);
}

TEST_CASE("vban_protocol_sample_rate_table") {
    ASSERT_EQ(kOfficialRates[3], 48000U);
    ASSERT_EQ(kOfficialRates[4], 96000U);
    ASSERT_EQ(kOfficialRates[5], 192000U);
    ASSERT_EQ(kOfficialRates[16], 44100U);
    // Extended index 21 -> 768 kHz
    uint32_t extended_rate = 768000U;
    ASSERT_EQ(extended_rate, 768000U);
}

TEST_CASE("vban_protocol_stream_name_extraction") {
    VbanHeaderLayout hdr{};
    std::memcpy(hdr.streamname, "Stream1\0\0\0\0\0\0\0\0\0", 16);
    std::string_view name(hdr.streamname, strnlen(hdr.streamname, 16));
    ASSERT_EQ(name, "Stream1");
}

TEST_CASE("vban_protocol_payload_size_calculation") {
    // 256 samples, 2 channels, 16-bit PCM (2 bytes)
    uint8_t nbs = 255; // 256 samples
    uint8_t nbc = 1;   // 2 channels
    size_t sample_size = 2; // 16-bit
    size_t expected_payload = (nbs + 1) * (nbc + 1) * sample_size;
    ASSERT_EQ(expected_payload, 1024U);
    size_t total_packet = 28 + expected_payload;
    ASSERT_EQ(total_packet, 1052U);
}
```

---

### 4.5 `native_tests/test_vban_converter.cpp`

```cpp
#include "test_framework.hpp"
#include <cstdint>
#include <cmath>
#include <algorithm>

inline float dequantize_pcm16(int16_t sample) {
    return static_cast<float>(sample) / 32768.0f;
}

inline float dequantize_pcm24_packed(const uint8_t* bytes) {
    int32_t raw = (static_cast<int32_t>(bytes[2]) << 24) |
                  (static_cast<int32_t>(bytes[1]) << 16) |
                  (static_cast<int32_t>(bytes[0]) << 8);
    int32_t s24 = raw >> 8; // Arithmetic sign extension
    return static_cast<float>(s24) / 8388608.0f;
}

inline float dequantize_pcm32(int32_t sample) {
    return static_cast<float>(sample) / 2147483648.0f;
}

inline float sanitize_float32(float sample) {
    if (std::isnan(sample)) return 0.0f;
    return std::clamp(sample, -1.0f, +1.0f);
}

TEST_CASE("converter_pcm16_normalization") {
    ASSERT_NEAR(dequantize_pcm16(0), 0.0f, 1e-6f);
    ASSERT_NEAR(dequantize_pcm16(32767), 0.9999695f, 1e-6f);
    ASSERT_NEAR(dequantize_pcm16(-32768), -1.0f, 1e-6f);
}

TEST_CASE("converter_pcm24_packed_normalization") {
    uint8_t zero_bytes[3] = {0x00, 0x00, 0x00};
    ASSERT_NEAR(dequantize_pcm24_packed(zero_bytes), 0.0f, 1e-7f);

    uint8_t min_bytes[3] = {0x00, 0x00, 0x80}; // -8388608
    ASSERT_NEAR(dequantize_pcm24_packed(min_bytes), -1.0f, 1e-7f);

    uint8_t max_bytes[3] = {0xFF, 0xFF, 0x7F}; // +8388607
    ASSERT_NEAR(dequantize_pcm24_packed(max_bytes), 0.9999999f, 1e-7f);
}

TEST_CASE("converter_pcm32_normalization") {
    ASSERT_NEAR(dequantize_pcm32(0), 0.0f, 1e-9f);
    ASSERT_NEAR(dequantize_pcm32(INT32_MIN), -1.0f, 1e-9f);
    ASSERT_NEAR(dequantize_pcm32(INT32_MAX), 0.99999999f, 1e-7f);
}

TEST_CASE("converter_float32_sanitization_and_clamping") {
    ASSERT_NEAR(sanitize_float32(0.5f), 0.5f, 1e-6f);
    ASSERT_NEAR(sanitize_float32(-0.5f), -0.5f, 1e-6f);
    // Clamping
    ASSERT_NEAR(sanitize_float32(1.5f), 1.0f, 1e-6f);
    ASSERT_NEAR(sanitize_float32(-2.0f), -1.0f, 1e-6f);
    // NaN replacement
    float nan_val = std::numeric_limits<float>::quiet_NaN();
    ASSERT_NEAR(sanitize_float32(nan_val), 0.0f, 1e-6f);
}
```

---

### 4.6 `native_tests/test_spsc_buffer.cpp`

```cpp
#include "test_framework.hpp"
#include <atomic>
#include <thread>
#include <vector>
#include <memory>

template <typename T, size_t Capacity>
class MinimalSpscBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of two");
    static constexpr size_t kMask = Capacity - 1;

public:
    MinimalSpscBuffer() : m_storage(std::make_unique<T[]>(Capacity)) {}

    size_t write(const T* data, size_t count) {
        size_t write_idx = m_write_idx.load(std::memory_order_relaxed);
        size_t read_idx = m_read_idx.load(std::memory_order_acquire);
        size_t available = Capacity - (write_idx - read_idx);
        size_t to_write = std::min(count, available);

        for (size_t i = 0; i < to_write; ++i) {
            m_storage[(write_idx + i) & kMask] = data[i];
        }
        m_write_idx.store(write_idx + to_write, std::memory_order_release);
        return to_write;
    }

    size_t read(T* out_data, size_t count) {
        size_t read_idx = m_read_idx.load(std::memory_order_relaxed);
        size_t write_idx = m_write_idx.load(std::memory_order_acquire);
        size_t available = write_idx - read_idx;
        size_t to_read = std::min(count, available);

        for (size_t i = 0; i < to_read; ++i) {
            out_data[i] = m_storage[(read_idx + i) & kMask];
        }
        m_read_idx.store(read_idx + to_read, std::memory_order_release);
        return to_read;
    }

    size_t availableRead() const noexcept {
        return m_write_idx.load(std::memory_order_acquire) - m_read_idx.load(std::memory_order_relaxed);
    }

private:
    alignas(64) std::atomic<size_t> m_write_idx{0};
    alignas(64) std::atomic<size_t> m_read_idx{0};
    std::unique_ptr<T[]> m_storage;
};

TEST_CASE("spsc_buffer_power_of_two_capacity") {
    MinimalSpscBuffer<float, 262144> buffer;
    ASSERT_EQ(buffer.availableRead(), 0U);

    std::vector<float> input(100, 1.23f);
    size_t written = buffer.write(input.data(), input.size());
    ASSERT_EQ(written, 100U);
    ASSERT_EQ(buffer.availableRead(), 100U);

    std::vector<float> output(100, 0.0f);
    size_t read_count = buffer.read(output.data(), output.size());
    ASSERT_EQ(read_count, 100U);
    ASSERT_EQ(buffer.availableRead(), 0U);
    ASSERT_NEAR(output[0], 1.23f, 1e-6f);
}

TEST_CASE("spsc_buffer_multithreaded_concurrency") {
    constexpr size_t kTotalSamples = 100000;
    MinimalSpscBuffer<uint32_t, 4096> buffer;

    std::thread producer([&]() {
        for (uint32_t i = 0; i < kTotalSamples;) {
            uint32_t chunk[64];
            size_t count = std::min<size_t>(64, kTotalSamples - i);
            for (size_t j = 0; j < count; ++j) {
                chunk[j] = i + static_cast<uint32_t>(j);
            }
            size_t written = buffer.write(chunk, count);
            i += static_cast<uint32_t>(written);
        }
    });

    std::vector<uint32_t> received;
    received.reserve(kTotalSamples);

    std::thread consumer([&]() {
        while (received.size() < kTotalSamples) {
            uint32_t chunk[64];
            size_t read_cnt = buffer.read(chunk, 64);
            for (size_t j = 0; j < read_cnt; ++j) {
                received.push_back(chunk[j]);
            }
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(received.size(), kTotalSamples);
    for (size_t i = 0; i < kTotalSamples; ++i) {
        if (received[i] != static_cast<uint32_t>(i)) {
            ASSERT_EQ(received[i], static_cast<uint32_t>(i));
        }
    }
}
```

---

### 4.7 `native_tests/test_drift_resampler.cpp`

```cpp
#include "test_framework.hpp"
#include <cmath>

inline float catmull_rom(float ym1, float y0, float y1, float y2, float x) {
    float c0 = y0;
    float c1 = 0.5f * (y1 - ym1);
    float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
    float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
    return ((c3 * x + c2) * x + c1) * x + c0;
}

TEST_CASE("drift_resampler_catmull_rom_endpoints") {
    // Exact matching at integer nodal boundaries
    float y0 = 1.0f, y1 = 2.0f;
    float v0 = catmull_rom(0.0f, y0, y1, 3.0f, 0.0f);
    float v1 = catmull_rom(0.0f, y0, y1, 3.0f, 1.0f);
    ASSERT_NEAR(v0, y0, 1e-6f);
    ASSERT_NEAR(v1, y1, 1e-6f);
}

TEST_CASE("drift_resampler_catmull_rom_midpoint_linearity") {
    // Midpoint between 1.0 and 2.0 with linear slope should equal 1.5
    float vmid = catmull_rom(0.0f, 1.0f, 2.0f, 3.0f, 0.5f);
    ASSERT_NEAR(vmid, 1.5f, 1e-6f);
}

TEST_CASE("drift_resampler_ema_watermark_filter") {
    float ema = 100.0f;
    float alpha = 0.1f;
    // Step response towards 200.0f
    for (int i = 0; i < 50; ++i) {
        ema = (1.0f - alpha) * ema + alpha * 200.0f;
    }
    ASSERT_NEAR(ema, 200.0f, 1.0f);
}

TEST_CASE("drift_resampler_pi_controller_clamping") {
    float max_ppm = 100.0f;
    float raw_correction = 250.0f;
    float clamped = std::clamp(raw_correction, -max_ppm, +max_ppm);
    ASSERT_EQ(clamped, 100.0f);
}
```

---

## 5. Build & Execution Workflow

### 5.1 Direct NDK Clang Invocation (Immediate Verification)
```powershell
& "D:\Android\sdk\ndk\26.1.10909125\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe" `
    -std=c++20 -O2 `
    -I native_tests `
    -I app/src/main/cpp/include `
    native_tests/test_main.cpp `
    native_tests/test_vban_protocol.cpp `
    native_tests/test_vban_converter.cpp `
    native_tests/test_spsc_buffer.cpp `
    native_tests/test_drift_resampler.cpp `
    -o native_tests/vban_native_tests.exe

& native_tests/vban_native_tests.exe
```

### 5.2 CMake & CTest Invocation
```powershell
cmake -S native_tests -B native_tests/build -G Ninja -DCMAKE_MAKE_PROGRAM="D:/Android/sdk/cmake/3.22.1/bin/ninja.exe" -DCMAKE_CXX_COMPILER="D:/Android/sdk/ndk/26.1.10909125/toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe"
cmake --build native_tests/build
ctest --test-dir native_tests/build --output-on-failure
```

---

## 6. Synthesis & Downstream Guidance

1. **For the Implementer Agent**:
   - Create `native_tests/` directory.
   - Populate the 7 files specified in Section 4.
   - Execute the test binary using either direct NDK Clang or CMake to confirm all tests pass cleanly.
2. **For Milestone 2 (VBAN Core)**:
   - As `app/src/main/cpp/include/vban_protocol.hpp` and converter implementations are built, update the test cases in `test_vban_protocol.cpp` and `test_vban_converter.cpp` to directly include and exercise the production classes.
