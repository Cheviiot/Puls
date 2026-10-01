# Chainloaded by the Qt for Android toolchain (QT_CHAINLOAD_TOOLCHAIN_FILE)
# after it selects ANDROID_ABI: picks the vcpkg triplet of the ABI and loads
# vcpkg, which loads the Android NDK toolchain. Qt passes this file to the
# builds of the other ABIs of a multi-ABI package as well; vcpkg alone would
# derive the triplet from the host processor.

if(NOT DEFINED VCPKG_TARGET_TRIPLET AND DEFINED ANDROID_ABI)
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        set(VCPKG_TARGET_TRIPLET arm64-android CACHE STRING "vcpkg target triplet")
    elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
        set(VCPKG_TARGET_TRIPLET arm-neon-android CACHE STRING "vcpkg target triplet")
    elseif(ANDROID_ABI STREQUAL "x86_64")
        set(VCPKG_TARGET_TRIPLET x64-android CACHE STRING "vcpkg target triplet")
    else()
        message(FATAL_ERROR "Unsupported Android ABI '${ANDROID_ABI}'")
    endif()
endif()
if(NOT DEFINED VCPKG_CHAINLOAD_TOOLCHAIN_FILE AND DEFINED ANDROID_NDK_ROOT)
    set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${ANDROID_NDK_ROOT}/build/cmake/android.toolchain.cmake"
        CACHE FILEPATH "Android NDK toolchain")
endif()
include("$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
