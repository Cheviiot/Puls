cmake_minimum_required(VERSION 3.25)

option(PULS_BUILD_TESTS "Build unit and mock-network tests" ON)
option(PULS_BUILD_GUI "Build the Qt Quick graphical interface" ON)
option(PULS_LIVE_TESTS "Build tests that contact the real measurement services" OFF)
option(PULS_SYSTEM_TLS_TESTS "Build a smoke test for the platform certificate store" OFF)
option(PULS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(PULS_SANITIZERS "" CACHE STRING "Semicolon-separated sanitizers: address, undefined, thread")
set(PULS_VERSION "dev" CACHE STRING "Version reported by `puls version`")

# Floating-point std::to_chars requires macOS 13.3; cmake/triplets builds the
# vcpkg dependencies for the same version.
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum macOS version")

# vcpkg reads manifest features while processing project().
if(PULS_BUILD_TESTS)
    list(APPEND VCPKG_MANIFEST_FEATURES "tests")
endif()
if(PULS_BUILD_GUI)
    list(APPEND VCPKG_MANIFEST_FEATURES "gui")
endif()

project(Puls
    DESCRIPTION "Internet latency and throughput measurement"
    HOMEPAGE_URL "https://github.com/Cheviiot/Puls"
    LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not supported; use a preset or -B build")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PulsCompilerOptions)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
find_package(OpenSSL 3.0 REQUIRED)
find_package(Boost 1.83 CONFIG REQUIRED COMPONENTS json)
if(PULS_BUILD_GUI)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)
endif()

add_subdirectory(src)

if(PULS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
