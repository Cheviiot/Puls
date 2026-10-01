find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# Local HTTPS/WebSocket servers and TLS fixtures.
add_library(puls_test_support STATIC support/mock_server.cpp)
target_include_directories(puls_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(puls_test_support PUBLIC Puls::net GTest::gtest)
puls_configure_target(puls_test_support)

# NO_MAIN tests define main() themselves.
function(puls_add_test name)
    cmake_parse_arguments(PARSE_ARGV 1 TEST "NO_MAIN" "" "SOURCES;LIBRARIES")
    add_executable(${name} ${TEST_SOURCES})
    if(TEST_NO_MAIN)
        target_link_libraries(${name} PRIVATE puls_test_support GTest::gtest ${TEST_LIBRARIES})
    else()
        target_link_libraries(${name} PRIVATE puls_test_support GTest::gtest_main ${TEST_LIBRARIES})
    endif()
    puls_configure_target(${name})
    if(MSVC)
        # Function names in the stack of a crashed test on CI.
        target_link_options(${name} PRIVATE /DEBUG)
    endif()
    gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 60
        PROPERTIES TIMEOUT 120)
endfunction()

puls_add_test(puls_core_tests SOURCES core_test.cpp LIBRARIES Puls::core)
puls_add_test(puls_net_tests SOURCES net_test.cpp LIBRARIES Puls::net)
puls_add_test(puls_measure_tests SOURCES measure_test.cpp LIBRARIES Puls::measure)
puls_add_test(puls_service_tests SOURCES service_test.cpp LIBRARIES Puls::service)
puls_add_test(puls_yandex_tests SOURCES yandex_test.cpp LIBRARIES Puls::yandex)
puls_add_test(puls_speedtestru_tests SOURCES speedtestru_test.cpp LIBRARIES Puls::speedtestru)
puls_add_test(puls_cli_tests SOURCES cli_test.cpp LIBRARIES Puls::cli)
# Stands in for puls-gui when the tests run the real puls executable.
add_executable(puls_fake_gui support/fake_gui.cpp)
puls_configure_target(puls_fake_gui)
add_dependencies(puls_cli_tests puls puls_fake_gui)
target_compile_definitions(puls_cli_tests PRIVATE
    PULS_CLI_PATH="$<TARGET_FILE:puls>"
    PULS_FAKE_GUI_PATH="$<TARGET_FILE:puls_fake_gui>")
puls_add_test(puls_gui_model_tests SOURCES gui_model_test.cpp LIBRARIES Puls::gui_model)
if(PULS_BUILD_GUI)
    puls_add_test(puls_gui_tests NO_MAIN SOURCES gui_test.cpp LIBRARIES Puls::gui)
    if(PULS_QT_STATIC)
        # The tests render without a display.
        set_target_properties(puls_gui_tests PROPERTIES
            QT_QML_ROOT_PATH "${PROJECT_SOURCE_DIR}/src/puls/gui/qml")
        qt_import_plugins(puls_gui_tests INCLUDE Qt6::QOffscreenIntegrationPlugin)
        qt_import_qml_plugins(puls_gui_tests)
    endif()
endif()

if(PULS_LIVE_TESTS)
    puls_add_test(puls_live_tests SOURCES live_test.cpp
        LIBRARIES Puls::yandex Puls::speedtestru)
endif()
if(PULS_SYSTEM_TLS_TESTS)
    puls_add_test(puls_system_tls_tests SOURCES system_tls_test.cpp LIBRARIES Puls::net)
endif()
