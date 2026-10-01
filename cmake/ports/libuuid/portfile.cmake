# The fontconfig port still depends on libuuid, but fontconfig 2.17 does not
# use it: its meson build never looks for the library. The upstream port
# downloads libuuid from SourceForge, which is not reliably available, and no
# other dependency of Puls needs it, so the package is intentionally empty.
set(VCPKG_POLICY_EMPTY_PACKAGE enabled)
