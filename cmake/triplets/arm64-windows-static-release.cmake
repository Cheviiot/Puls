# arm64-windows-static for Release builds: static libraries and runtime, so
# the executables need no DLLs. MSVC does not mix debug and release runtimes,
# so only the Debug preset needs debug builds of the dependencies.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_PROVIDED_FORTRAN ON)
set(VCPKG_BUILD_TYPE release)

# OpenSSL compiles with /Gs0, a stack probe in every function. The ARM64
# compiler then calls __chkstk before some functions save the link register,
# and they return into themselves (an access violation in the TLS handshake).
# The default threshold of one page, given after /Gs0, keeps the probe out
# of small frames.
if(PORT STREQUAL "openssl")
    set(VCPKG_C_FLAGS_RELEASE "/Gs4096")
endif()
