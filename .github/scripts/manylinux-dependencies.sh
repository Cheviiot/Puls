#!/bin/sh
# Installs what vcpkg needs to build Qt in the manylinux_2_28 (AlmaLinux 8)
# container: X11 and xcb headers, autotools, Perl with its standard modules
# for OpenSSL, and fonts for the GUI tests. The container provides GCC from
# gcc-toolset.
set -eu

dnf install -y dnf-plugins-core epel-release
dnf config-manager --set-enabled powertools
dnf install -y \
  autoconf autoconf-archive automake libtool perl zip unzip \
  fontconfig dejavu-sans-fonts \
  libX11-devel libXext-devel libXi-devel libXrender-devel libxcb-devel \
  libxkbcommon-devel libxkbcommon-x11-devel mesa-libEGL-devel mesa-libGL-devel \
  xcb-util-cursor-devel xcb-util-devel xcb-util-image-devel \
  xcb-util-keysyms-devel xcb-util-renderutil-devel xcb-util-wm-devel
