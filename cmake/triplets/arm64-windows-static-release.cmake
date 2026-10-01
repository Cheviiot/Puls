# arm64-windows-static for Release builds: static libraries and runtime, so
# the executables need no DLLs. MSVC does not mix debug and release runtimes,
# so only the Debug preset needs debug builds of the dependencies.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_PROVIDED_FORTRAN ON)
set(VCPKG_BUILD_TYPE release)
