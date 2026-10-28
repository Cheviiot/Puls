if(NOT PULS_DEPLOY_RUNTIME)
    return()
endif()

if(ANDROID)
    message(FATAL_ERROR "PULS_DEPLOY_RUNTIME is for native desktop targets; Android uses androiddeployqt")
endif()
if(Qt6_VERSION VERSION_LESS 6.8)
    message(FATAL_ERROR "PULS_DEPLOY_RUNTIME requires Qt 6.8 or later")
endif()

# Deployment uses a complete prefix-relative tree. Absolute GNUInstallDirs paths
# would escape --prefix/DESTDIR and prevent relocation of an archive.
foreach(directory IN ITEMS CMAKE_INSTALL_BINDIR CMAKE_INSTALL_LIBDIR
        CMAKE_INSTALL_DATADIR CMAKE_INSTALL_DOCDIR)
    if(IS_ABSOLUTE "${${directory}}" OR "${${directory}}" MATCHES "(^|/)\\.\\.(/|$)")
        message(FATAL_ERROR "${directory} must be prefix-relative for PULS_DEPLOY_RUNTIME")
    endif()
endforeach()

set(PULS_RUNTIME_PLUGINS_DIR "plugins")
if(APPLE AND PULS_BUILD_GUI)
    find_program(PULS_SIPS_EXECUTABLE sips REQUIRED)
    find_program(PULS_ICONUTIL_EXECUTABLE iconutil REQUIRED)
    set(puls_bundle_icon "${PROJECT_BINARY_DIR}/Puls.icns")
    add_custom_command(OUTPUT "${puls_bundle_icon}"
        COMMAND "${CMAKE_COMMAND}"
            "-DPULS_ICON_INPUT=${PROJECT_SOURCE_DIR}/internal/gui/assets/Icon.png"
            "-DPULS_ICON_OUTPUT=${puls_bundle_icon}"
            "-DPULS_SIPS=${PULS_SIPS_EXECUTABLE}"
            "-DPULS_ICONUTIL=${PULS_ICONUTIL_EXECUTABLE}"
            -P "${PROJECT_SOURCE_DIR}/cmake/PulsMacIcon.cmake"
        DEPENDS "${PROJECT_SOURCE_DIR}/internal/gui/assets/Icon.png"
            "${PROJECT_SOURCE_DIR}/cmake/PulsMacIcon.cmake"
        VERBATIM)
    set_source_files_properties("${puls_bundle_icon}" PROPERTIES GENERATED TRUE
        MACOSX_PACKAGE_LOCATION Resources)
    target_sources(puls PRIVATE "${puls_bundle_icon}")
    set_target_properties(puls PROPERTIES MACOSX_BUNDLE_ICON_FILE Puls.icns)
endif()
if(UNIX AND NOT APPLE)
    find_program(PULS_PATCHELF_EXECUTABLE patchelf REQUIRED)
    file(RELATIVE_PATH puls_library_path "${PROJECT_BINARY_DIR}/${CMAKE_INSTALL_BINDIR}"
        "${PROJECT_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR}")
    set_target_properties(puls PROPERTIES
        INSTALL_RPATH "$ORIGIN/${puls_library_path}"
        INSTALL_RPATH_USE_LINK_PATH FALSE)
endif()

# Qt's SDK plugin path can be lib64/qt6/plugins (system Fedora SDK) instead of
# plugins (online SDK). Keep one archive layout and tell the official deployment
# script before it reads QT_DEPLOY_SUPPORT. No unsupported-platform suppression:
# native deployment must fail rather than create an incomplete release archive.
install(CODE "set(QT_DEPLOY_PLUGINS_DIR \"${PULS_RUNTIME_PLUGINS_DIR}\")")
set(puls_deploy_options "")
if(UNIX AND NOT APPLE)
    # The runtime relies on the target distribution's libc/ELF loader and trust
    # store. Never redistribute a different glibc next to the application.
    set(puls_system_library_patterns "ld-linux" "ld64[.]so" "libc[.]so" "libm[.]so"
        "libpthread[.]so" "librt[.]so" "libdl[.]so" "libresolv[.]so" "libutil[.]so"
        "libnss_" "libGL" "libEGL" "libOpenGL" "libvulkan" "libdrm" "libgbm"
        "libgtk-" "libgdk-3[.]so")
    set(puls_pre_exclude "")
    set(puls_post_exclude "")
    foreach(pattern IN LISTS puls_system_library_patterns)
        list(APPEND puls_pre_exclude "^${pattern}.*")
        list(APPEND puls_post_exclude "/${pattern}[^/]*$")
    endforeach()
    list(APPEND puls_deploy_options PRE_EXCLUDE_REGEXES ${puls_pre_exclude})
    # Qt normally ignores the compiler's system library paths. System SDKs on
    # merged-/usr distributions put Qt itself there. Explicit post exclusions
    # retain Qt and its runtime dependencies without bundling libc or drivers.
    list(APPEND puls_deploy_options POST_EXCLUDE_REGEXES ${puls_post_exclude})
endif()
if(PULS_BUILD_GUI AND NOT Qt6_VERSION VERSION_LESS 6.10 AND NOT APPLE)
    # Qt 6.10+ narrows QPA selection to SDK defaults. Preserve the
    # display-free preview platform in Linux and Windows packages.
    list(APPEND puls_deploy_options INCLUDE_PLUGINS qoffscreen qminimal)
    if(UNIX)
        list(APPEND puls_deploy_options INCLUDE_PLUGINS qwayland EXCLUDE_PLUGINS qgtk3)
    endif()
endif()
qt_generate_deploy_app_script(
    TARGET puls
    OUTPUT_SCRIPT puls_deploy_script
    NO_TRANSLATIONS
    ${puls_deploy_options})
install(SCRIPT "${puls_deploy_script}")

if(UNIX AND NOT APPLE)
    configure_file("${PROJECT_SOURCE_DIR}/cmake/PulsFixupLinux.cmake.in"
        "${PROJECT_BINARY_DIR}/PulsFixupLinux.cmake" @ONLY)
    install(SCRIPT "${PROJECT_BINARY_DIR}/PulsFixupLinux.cmake")
endif()

if(NOT APPLE)
    file(RELATIVE_PATH PULS_QT_PREFIX "${PROJECT_BINARY_DIR}/${CMAKE_INSTALL_BINDIR}"
        "${PROJECT_BINARY_DIR}")
    string(REGEX REPLACE "/+$" "" PULS_QT_PREFIX "${PULS_QT_PREFIX}")
    if(PULS_QT_PREFIX STREQUAL "")
        set(PULS_QT_PREFIX ".")
    endif()
    configure_file("${PROJECT_SOURCE_DIR}/cpp/qt.conf.in" "${PROJECT_BINARY_DIR}/puls-qt.conf" @ONLY)
    # Keep this after Qt deployment: windeployqt and the Linux deployment script
    # may write qt.conf too. This version also works with system-installed SDKs.
    install(FILES "${PROJECT_BINARY_DIR}/puls-qt.conf"
        DESTINATION "${CMAKE_INSTALL_BINDIR}" RENAME qt.conf)
endif()

if(APPLE)
    set(puls_doc_dir "puls.app/Contents/Resources")
    set(puls_license_dir "puls.app/Contents/Resources/licenses")
else()
    set(puls_doc_dir "${CMAKE_INSTALL_DOCDIR}")
    set(puls_license_dir "${CMAKE_INSTALL_DATADIR}/licenses/puls")
endif()
install(FILES "${PROJECT_SOURCE_DIR}/README.md" "${PROJECT_SOURCE_DIR}/CHANGELOG.md"
    DESTINATION "${puls_doc_dir}")
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION "${puls_license_dir}")
install(FILES "${PROJECT_SOURCE_DIR}/internal/gui/assets/Icon.png"
    "${PROJECT_SOURCE_DIR}/internal/gui/assets/Icon.svg"
    "${PROJECT_SOURCE_DIR}/internal/gui/assets/Icon.ico"
    DESTINATION assets)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/cmake/qt-licenses/"
    DESTINATION "${puls_license_dir}/Qt")

# Preserve the SDK's own extra notices where available. PULS_QT_LICENSE_DIR lets
# a native release job point at the exact SDK license/attribution directory.
set(PULS_QT_LICENSE_DIR "" CACHE PATH "Additional license and attribution files from the deployed Qt SDK")
set(puls_qt_license_directories "")
if(PULS_QT_LICENSE_DIR)
    if(NOT IS_DIRECTORY "${PULS_QT_LICENSE_DIR}")
        message(FATAL_ERROR "PULS_QT_LICENSE_DIR is not a directory")
    endif()
    list(APPEND puls_qt_license_directories "${PULS_QT_LICENSE_DIR}")
endif()
foreach(candidate IN ITEMS "${QT6_INSTALL_PREFIX}/LICENSES" "${QT6_INSTALL_PREFIX}/licenses")
    if(IS_DIRECTORY "${candidate}")
        list(APPEND puls_qt_license_directories "${candidate}")
    endif()
endforeach()
file(GLOB puls_qt_system_licenses LIST_DIRECTORIES TRUE "${QT6_INSTALL_PREFIX}/share/licenses/qt6-*")
list(APPEND puls_qt_license_directories ${puls_qt_system_licenses})
list(REMOVE_DUPLICATES puls_qt_license_directories)
foreach(directory IN LISTS puls_qt_license_directories)
    if(IS_DIRECTORY "${directory}")
        get_filename_component(name "${directory}" NAME)
        install(DIRECTORY "${directory}/" DESTINATION "${puls_license_dir}/Qt/sdk/${name}")
    endif()
endforeach()
configure_file("${PROJECT_SOURCE_DIR}/cmake/QtRuntimeNotice.txt.in"
    "${PROJECT_BINARY_DIR}/QtRuntimeNotice.txt" @ONLY)
install(FILES "${PROJECT_BINARY_DIR}/QtRuntimeNotice.txt" DESTINATION "${puls_license_dir}")
