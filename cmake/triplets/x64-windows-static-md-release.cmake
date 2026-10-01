# x64-windows-static-md for Release builds: MSVC does not mix debug and release
# runtimes, so only the Debug preset needs debug builds of the dependencies.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_PROVIDED_FORTRAN ON)
set(VCPKG_BUILD_TYPE release)

if(PORT MATCHES "^qt")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
