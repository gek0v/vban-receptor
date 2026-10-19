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
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_vban_protocol.cpp")
    list(APPEND TEST_SOURCES test_vban_protocol.cpp)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_vban_converter.cpp")
    list(APPEND TEST_SOURCES test_vban_converter.cpp)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_spsc_buffer.cpp")
    list(APPEND TEST_SOURCES test_spsc_buffer.cpp)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_drift_resampler.cpp")
    list(APPEND TEST_SOURCES test_drift_resampler.cpp)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_challenger_stress.cpp")
    list(APPEND TEST_SOURCES test_challenger_stress.cpp)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_vban_receiver.cpp")
    list(APPEND TEST_SOURCES test_vban_receiver.cpp)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_fft_analyzer.cpp")
    list(APPEND TEST_SOURCES test_fft_analyzer.cpp)
endif()

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
if(EXISTS "${VBAN_SRC_DIR}/vban_receiver.cpp")
    list(APPEND CORE_SOURCES "${VBAN_SRC_DIR}/vban_receiver.cpp")
endif()
if(EXISTS "${VBAN_SRC_DIR}/fft_analyzer.cpp")
    list(APPEND CORE_SOURCES "${VBAN_SRC_DIR}/fft_analyzer.cpp")
endif()

add_executable(vban_native_tests ${TEST_SOURCES} ${CORE_SOURCES})

target_include_directories(vban_native_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${VBAN_INCLUDE_DIR}"
)

# Multi-threading support for SPSC lock-free concurrency stress tests
find_package(Threads REQUIRED)
target_link_libraries(vban_native_tests PRIVATE Threads::Threads)
if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0600 NOMINMAX)
    target_link_libraries(vban_native_tests PRIVATE ws2_32)
endif()

# Compiler warnings and strict conformance
if(MSVC)
    target_compile_options(vban_native_tests PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(vban_native_tests PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter -O3)
endif()

# Register test with CTest
add_test(NAME vban_tests COMMAND vban_native_tests)
