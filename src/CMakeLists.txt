# Core utilities shared by every layer: errors, cancellation, interrupts,
# text, JSON.
add_library(puls_core STATIC
    puls/core/context.cpp
    puls/core/error.cpp
    puls/core/interrupt.cpp
    puls/core/ip.cpp
    puls/core/json.cpp
    puls/core/text.cpp)
add_library(Puls::core ALIAS puls_core)
target_include_directories(puls_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(puls_core PUBLIC Boost::headers Boost::json Threads::Threads)
puls_configure_target(puls_core)

# Throughput-measurement engine.
add_library(puls_measure STATIC puls/measure/measure.cpp)
add_library(Puls::measure ALIAS puls_measure)
target_link_libraries(puls_measure PUBLIC Puls::core)
puls_configure_target(puls_measure)

# HTTP/1.1 and WebSocket clients over Boost.Beast and OpenSSL.
add_library(puls_net STATIC
    puls/net/detail/transport.cpp
    puls/net/http.cpp
    puls/net/tls.cpp
    puls/net/url.cpp
    puls/net/websocket.cpp)
add_library(Puls::net ALIAS puls_net)
target_link_libraries(puls_net PUBLIC Puls::core OpenSSL::SSL OpenSSL::Crypto)
if(WIN32)
    target_link_libraries(puls_net PUBLIC ws2_32 mswsock crypt32)
elseif(APPLE)
    target_link_libraries(puls_net PUBLIC "-framework CoreFoundation" "-framework Security")
endif()
puls_configure_target(puls_net)

# Shared service contracts, error classification, statistics and HTTP helpers.
add_library(puls_service STATIC
    puls/service/http.cpp
    puls/service/service.cpp)
add_library(Puls::service ALIAS puls_service)
target_link_libraries(puls_service PUBLIC Puls::core Puls::measure Puls::net)
puls_configure_target(puls_service)

# Яндекс.Интернетометр protocol.
add_library(puls_yandex STATIC
    puls/service/yandex/connection.cpp
    puls/service/yandex/discovery.cpp
    puls/service/yandex/ping.cpp
    puls/service/yandex/protocol.cpp
    puls/service/yandex/throughput.cpp
    puls/service/yandex/upload.cpp)
add_library(Puls::yandex ALIAS puls_yandex)
target_link_libraries(puls_yandex PUBLIC Puls::service)
puls_configure_target(puls_yandex)

# speedtest.ru QMS protocol.
add_library(puls_speedtestru STATIC
    puls/service/speedtestru/authorization.cpp
    puls/service/speedtestru/backend.cpp
    puls/service/speedtestru/connection.cpp
    puls/service/speedtestru/discovery.cpp
    puls/service/speedtestru/ping.cpp
    puls/service/speedtestru/throughput.cpp)
add_library(Puls::speedtestru ALIAS puls_speedtestru)
target_link_libraries(puls_speedtestru PUBLIC Puls::service)
puls_configure_target(puls_speedtestru)

# Orchestration shared by the CLI and the graphical interface.
add_library(puls_application STATIC
    puls/application/runner.cpp
    puls/application/types.cpp)
add_library(Puls::application ALIAS puls_application)
target_link_libraries(puls_application PUBLIC Puls::service Puls::yandex Puls::speedtestru)
puls_configure_target(puls_application)

# Dashboard state and behavior shared by graphical frontends; no toolkit.
add_library(puls_gui_model STATIC puls/gui/model.cpp)
add_library(Puls::gui_model ALIAS puls_gui_model)
target_link_libraries(puls_gui_model PUBLIC Puls::application)
puls_configure_target(puls_gui_model)

# Terminal output, colors, progress and the interactive menu.
add_library(puls_ui STATIC
    puls/ui/select.cpp
    puls/ui/terminal.cpp)
add_library(Puls::ui ALIAS puls_ui)
target_link_libraries(puls_ui PUBLIC Puls::core)
puls_configure_target(puls_ui)

# Command-line parsing, rendering and orchestration.
add_library(puls_cli STATIC
    puls/cli/application.cpp
    puls/cli/config.cpp
    puls/cli/gui_launcher.cpp
    puls/cli/render.cpp)
add_library(Puls::cli ALIAS puls_cli)
target_link_libraries(puls_cli PUBLIC Puls::application Puls::ui)
puls_configure_target(puls_cli)

# Android has only the graphical application.
if(NOT ANDROID)
    add_executable(puls puls/cli/main.cpp)
    target_link_libraries(puls PRIVATE Puls::cli)
    target_compile_definitions(puls PRIVATE PULS_VERSION="${PULS_VERSION}")
    if(WIN32)
        target_link_libraries(puls PRIVATE shell32)
        puls_add_windows_resources(puls "puls.exe" "Puls — проверка скорости интернета")
    endif()
    puls_configure_target(puls)
    install(TARGETS puls RUNTIME DESTINATION "${PULS_INSTALL_BINDIR}")
endif()

if(PULS_BUILD_GUI)
    # The QML module "Puls" with the views of the dashboard. Its build
    # directory must not be named "puls" like the executable.
    add_subdirectory(puls/gui/qml gui_qml)

    # Qt Quick dashboard over the dashboard model.
    add_library(puls_gui STATIC
        puls/gui/controller.cpp
        puls/gui/run.cpp
        puls/gui/settings_store.cpp)
    add_library(Puls::gui ALIAS puls_gui)
    set_target_properties(puls_gui PROPERTIES AUTOMOC ON)
    target_link_libraries(puls_gui PUBLIC
        Puls::gui_model puls_gui_qml Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick)
    if(PULS_QT_STATIC)
        target_compile_definitions(puls_gui PRIVATE PULS_QT_STATIC)
    endif()
    qt_add_resources(puls_gui "puls_gui_resources"
        PREFIX "/puls"
        BASE puls/gui
        FILES puls/gui/assets/Icon.png)
    puls_configure_target(puls_gui)

    # The graphical application; `puls gui` starts it.
    qt_add_executable(puls-gui WIN32 MACOSX_BUNDLE puls/gui/main.cpp)
    target_link_libraries(puls-gui PRIVATE Puls::gui Puls::ui)
    target_compile_definitions(puls-gui PRIVATE PULS_VERSION="${PULS_VERSION}")
    # The QML imports decide which QML plugins are linked statically
    # (qmlimportscanner) or deployed into the Android package
    # (androiddeployqt).
    set_target_properties(puls-gui PROPERTIES
        QT_QML_IMPORT_SCANNER_EXTRA_ROOT_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/puls/gui/qml"
        QT_QML_ROOT_PATH "${CMAKE_CURRENT_SOURCE_DIR}/puls/gui/qml")
    if(PULS_QT_STATIC)
        qt_import_qml_plugins(puls-gui)
        if(TARGET Qt6::QXcbGlxIntegrationPlugin)
            qt_import_plugins(puls-gui
                INCLUDE_BY_TYPE xcbglintegrations Qt6::QXcbGlxIntegrationPlugin)
        endif()
    endif()
    if(WIN32)
        puls_add_windows_resources(puls-gui "puls-gui.exe" "Puls — проверка скорости интернета")
    elseif(APPLE)
        puls_configure_macos_bundle(puls-gui)
    elseif(ANDROID)
        # The Android paths below are plain JSON strings.
        qt_policy(SET QTP0002 NEW)
        # Android installs only a greater version code over the installed app.
        # The version numbering restarted at 0.1.0 after 0.3.3, whose code was
        # 3003, so the codes start at 10000.
        math(EXPR puls_version_code
            "10000 + ${PULS_VERSION_MAJOR} * 1000000 + ${PULS_VERSION_MINOR} * 1000 + ${PULS_VERSION_PATCH}")
        # The package name and the signing key of the earlier releases let
        # the APK update the installed app.
        set_target_properties(puls-gui PROPERTIES
            QT_ANDROID_PACKAGE_NAME io.github.cheviiot.puls
            QT_ANDROID_APP_NAME Puls
            QT_ANDROID_APP_ICON "@mipmap/ic_launcher"
            QT_ANDROID_VERSION_CODE ${puls_version_code}
            QT_ANDROID_VERSION_NAME "${PULS_VERSION}"
            QT_ANDROID_MIN_SDK_VERSION 28
            QT_ANDROID_TARGET_SDK_VERSION 36
            # The APK is downloaded directly rather than from a store, so its
            # libraries are compressed.
            QT_ANDROID_LEGACY_PACKAGING ON
            QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/puls/gui/android")
        qt_add_android_permission(puls-gui NAME android.permission.INTERNET)
        # Devices with 16 KB memory pages load only libraries aligned for them.
        # The NDK compiles with debug information, which the release package
        # does not need.
        target_link_options(puls-gui PRIVATE "LINKER:-z,max-page-size=16384"
            "$<$<CONFIG:Release>:-Wl,--strip-debug>")
    endif()
    puls_configure_target(puls-gui)
    # On Android the APK is the package, so nothing is installed.
    if(NOT ANDROID)
        install(TARGETS puls-gui
            RUNTIME DESTINATION "${PULS_INSTALL_BINDIR}"
            BUNDLE DESTINATION "${PULS_INSTALL_BUNDLEDIR}")
    endif()
    if(APPLE)
        # Stripping during installation invalidates the linker signature.
        install(CODE "execute_process(COMMAND codesign --force --sign -
            \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${PULS_INSTALL_BUNDLEDIR}/Puls.app\"
            COMMAND_ERROR_IS_FATAL ANY)")
    elseif(UNIX AND NOT ANDROID)
        if(PULS_PORTABLE_INSTALL)
            # The installer creates the menu entry with this icon.
            install(FILES puls/gui/assets/Icon.png DESTINATION assets)
        else()
            install(FILES puls/gui/io.github.cheviiot.puls.desktop
                DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
            install(FILES puls/gui/assets/Icon.png
                DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps"
                RENAME io.github.cheviiot.puls.png)
        endif()
    endif()
endif()

install(FILES
    "${PROJECT_SOURCE_DIR}/CHANGELOG.md"
    "${PROJECT_SOURCE_DIR}/LICENSE"
    "${PROJECT_SOURCE_DIR}/README.md"
    DESTINATION "${PULS_INSTALL_DOCDIR}")
if(PULS_PORTABLE_INSTALL)
    puls_add_third_party_notices()
endif()
