# Installation layout and platform metadata of the executables: Windows
# version resources, the macOS application bundle and license notices.

include(GNUInstallDirs)

if(PULS_PORTABLE_INSTALL)
    # Release archives keep the executables and documents at their root.
    set(PULS_INSTALL_BINDIR ".")
    set(PULS_INSTALL_DOCDIR ".")
    set(PULS_INSTALL_BUNDLEDIR ".")
else()
    set(PULS_INSTALL_BINDIR "${CMAKE_INSTALL_BINDIR}")
    set(PULS_INSTALL_DOCDIR "${CMAKE_INSTALL_DOCDIR}")
    set(PULS_INSTALL_BUNDLEDIR "Applications")
endif()

if(PULS_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(PULS_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(PULS_VERSION_MINOR "${CMAKE_MATCH_2}")
    set(PULS_VERSION_PATCH "${CMAKE_MATCH_3}")
else()
    set(PULS_VERSION_MAJOR 0)
    set(PULS_VERSION_MINOR 0)
    set(PULS_VERSION_PATCH 0)
endif()

set(PULS_ASSETS_DIR "${PROJECT_SOURCE_DIR}/src/puls/gui/assets")

# Adds the icon and version information to a Windows executable.
function(puls_add_windows_resources target file_name description)
    set(PULS_RC_FILE_NAME "${file_name}")
    set(PULS_RC_DESCRIPTION "${description}")
    set(PULS_RC_ICON "${PULS_ASSETS_DIR}/Icon.ico")
    set(resource "${CMAKE_CURRENT_BINARY_DIR}/${target}.rc")
    configure_file("${PROJECT_SOURCE_DIR}/cmake/Puls.rc.in" "${resource}" @ONLY)
    target_sources(${target} PRIVATE "${resource}")
endfunction()

# Makes target the Puls.app bundle with its icon and Info.plist.
function(puls_configure_macos_bundle target)
    set(PULS_BUNDLE_VERSION "${PULS_VERSION_MAJOR}.${PULS_VERSION_MINOR}.${PULS_VERSION_PATCH}")
    set(plist "${CMAKE_CURRENT_BINARY_DIR}/Info.plist")
    configure_file("${PROJECT_SOURCE_DIR}/cmake/Info.plist.in" "${plist}" @ONLY)

    set(icon_set "${CMAKE_CURRENT_BINARY_DIR}/Puls.iconset")
    set(icon "${CMAKE_CURRENT_BINARY_DIR}/Puls.icns")
    set(source "${PULS_ASSETS_DIR}/Icon.png")
    set(resize_commands)
    foreach(size IN ITEMS 16 32 128 256 512)
        list(APPEND resize_commands
            COMMAND sips -z ${size} ${size} "${source}" --out "${icon_set}/icon_${size}x${size}.png")
        math(EXPR double "${size} * 2")
        if(double LESS_EQUAL 512)
            list(APPEND resize_commands
                COMMAND sips -z ${double} ${double} "${source}"
                    --out "${icon_set}/icon_${size}x${size}@2x.png")
        endif()
    endforeach()
    add_custom_command(OUTPUT "${icon}"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${icon_set}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${icon_set}"
        ${resize_commands}
        COMMAND iconutil --convert icns --output "${icon}" "${icon_set}"
        DEPENDS "${source}"
        VERBATIM)
    target_sources(${target} PRIVATE "${icon}")
    set_source_files_properties("${icon}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME Puls
        MACOSX_BUNDLE_INFO_PLIST "${plist}")
endfunction()

# Writes THIRD_PARTY_NOTICES.txt from the license files of the vcpkg ports
# the executables are built with.
function(puls_add_third_party_notices)
    if(NOT DEFINED VCPKG_INSTALLED_DIR OR NOT DEFINED VCPKG_TARGET_TRIPLET)
        return()
    endif()
    file(READ "${PROJECT_SOURCE_DIR}/vcpkg.json" manifest)
    string(JSON baseline GET "${manifest}" builtin-baseline)
    set(notices "${PROJECT_BINARY_DIR}/THIRD_PARTY_NOTICES.txt")
    add_custom_command(OUTPUT "${notices}"
        COMMAND "${CMAKE_COMMAND}"
            "-DPACKAGES_DIR=${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}"
            "-DVCPKG_BASELINE=${baseline}"
            "-DOUTPUT=${notices}"
            -P "${PROJECT_SOURCE_DIR}/cmake/PulsNotices.cmake"
        DEPENDS "${PROJECT_SOURCE_DIR}/cmake/PulsNotices.cmake" "${PROJECT_SOURCE_DIR}/vcpkg.json"
        VERBATIM)
    add_custom_target(puls_third_party_notices ALL DEPENDS "${notices}")
    install(FILES "${notices}" DESTINATION "${PULS_INSTALL_DOCDIR}")
endfunction()
