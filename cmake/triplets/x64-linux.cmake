# Builtin x64-linux with Qt as shared libraries: the LGPL lets users replace
# them, and Qt loads its platform and QML plugins at run time.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Linux)

if(PORT MATCHES "^qt")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
