# Community x64-osx with the macOS version that Puls supports (floating-point
# std::to_chars requires macOS 13.3) and Qt as shared libraries: the LGPL lets
# users replace them, and Qt loads its platform and QML plugins at run time.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES x86_64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 13.3)

if(PORT MATCHES "^qt")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
