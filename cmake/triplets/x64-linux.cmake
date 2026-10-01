# Builtin x64-linux with Qt as shared libraries: the LGPL lets users replace
# them, and Qt loads its platform and QML plugins at run time.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Linux)

# Dependencies are built only in Release: debug builds of Puls use them like
# system libraries, and Qt takes half as long to build.
set(VCPKG_BUILD_TYPE release)

if(PORT MATCHES "^qt")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
