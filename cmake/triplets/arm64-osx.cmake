# Builtin arm64-osx with the macOS version that Puls supports (floating-point
# std::to_chars requires macOS 13.3) and the dependencies built only in
# Release: debug builds of Puls use them like system libraries.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 13.3)
set(VCPKG_BUILD_TYPE release)
