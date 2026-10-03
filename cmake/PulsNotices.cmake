# Collects the license files of the vcpkg ports into one notices file.
# Ports with the same license text share a section; build tools are skipped.
# GUI_ASSETS_DIR adds the fonts and icons of the graphical interface.
#
# cmake -DPACKAGES_DIR=<vcpkg_installed>/<triplet> -DVCPKG_BASELINE=<commit>
#       -DOUTPUT=<file> [-DGUI_ASSETS_DIR=<src/puls/gui/qml>] -P PulsNotices.cmake

foreach(variable IN ITEMS PACKAGES_DIR VCPKG_BASELINE OUTPUT)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()

file(GLOB license_files LIST_DIRECTORIES false "${PACKAGES_DIR}/share/*/copyright")
list(SORT license_files)

set(hashes)
foreach(license_file IN LISTS license_files)
    get_filename_component(port_dir "${license_file}" DIRECTORY)
    get_filename_component(port "${port_dir}" NAME)
    if(port MATCHES "^(vcpkg-.*|gperf|pkgconf|gtest)$")
        continue()
    endif()
    file(SHA256 "${license_file}" hash)
    if(NOT DEFINED ports_${hash})
        list(APPEND hashes ${hash})
        set(file_${hash} "${license_file}")
    endif()
    list(APPEND ports_${hash} ${port})
endforeach()
if(NOT hashes)
    message(FATAL_ERROR "No license files in ${PACKAGES_DIR}/share")
endif()

set(text "Third-party software in Puls

Puls is distributed under the MIT License, see LICENSE. Its executables
include the libraries listed below, built from source with vcpkg
(https://github.com/microsoft/vcpkg) at baseline ${VCPKG_BASELINE}.

The corresponding source code, including the vcpkg port patches, is
available from https://github.com/Cheviiot/Puls and
https://github.com/microsoft/vcpkg/tree/${VCPKG_BASELINE}. Qt is used under
the GNU Lesser General Public License version 3: to use Puls with a
modified Qt, build Puls from source against it as described in the Puls
repository.
")
foreach(hash IN LISTS hashes)
    list(JOIN ports_${hash} ", " names)
    file(READ "${file_${hash}}" license)
    string(APPEND text "\n================================================================\n"
        "${names}\n"
        "================================================================\n\n"
        "${license}\n")
endforeach()

if(DEFINED GUI_ASSETS_DIR)
    string(APPEND text "\nThe graphical interface also includes a subset of the Inter typeface\n"
        "(https://rsms.me/inter/) and icons of Lucide (https://lucide.dev/).\n")
    foreach(asset IN ITEMS "Inter 4.1;fonts/LICENSE.txt" "Lucide 1.50.0;LICENSE.lucide.txt")
        list(GET asset 0 name)
        list(GET asset 1 license_file)
        file(READ "${GUI_ASSETS_DIR}/${license_file}" license)
        string(APPEND text "\n================================================================\n"
            "${name}\n"
            "================================================================\n\n"
            "${license}\n")
    endforeach()
endif()
file(WRITE "${OUTPUT}" "${text}")
