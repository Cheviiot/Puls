# Builtin x64-windows-static-md with Qt as DLLs: the LGPL lets users replace
# them, and Qt loads its platform and QML plugins at run time.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_PROVIDED_FORTRAN ON)

if(PORT MATCHES "^qt")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
