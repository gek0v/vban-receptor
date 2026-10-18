# ==============================================================================
# VBAN Receptor - Android Native Audio Engine Build Configuration
# ==============================================================================
cmake_minimum_required(VERSION 3.22.1)

project("vban_engine" CXX)

# Enforce standard ISO C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Locate Prefab-provided Oboe audio library
# Requires buildFeatures.prefab = true and com.google.oboe:oboe:1.9.0 in app/build.gradle.kts
find_package(oboe REQUIRED CONFIG)

# Native include directory
set(VBAN_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Define shared library target
add_library(vban_engine SHARED
    src/vban_engine.cpp
    src/vban_parser.cpp
    src/vban_converter.cpp
    src/drift_compensator.cpp
    src/oboe_player.cpp
    src/vban_receiver.cpp
    src/jni_bridge.cpp
    src/fft_analyzer.cpp
)

# Export include directories
target_include_directories(vban_engine PUBLIC
    ${VBAN_INCLUDE_DIR}
)

# Compiler warnings and optimization flags
target_compile_options(vban_engine PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror=return-type
    -O3
    -fvisibility=hidden
    -fvisibility-inlines-hidden
)

# Link dependencies
target_link_libraries(vban_engine PRIVATE
    oboe::oboe
    log
    android
)
