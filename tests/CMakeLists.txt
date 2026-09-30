find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# Local HTTPS/WebSocket servers and TLS fixtures.
add_library(puls_test_support STATIC support/mock_server.cpp)
target_include_directories(puls_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(puls_test_support PUBLIC Puls::net GTest::gtest)
puls_configure_target(puls_test_support)

function(puls_add_test name)
    cmake_parse_arguments(PARSE_ARGV 1 TEST "" "" "SOURCES;LIBRARIES")
    add_executable(${name} ${TEST_SOURCES})
    target_link_libraries(${name} PRIVATE puls_test_support GTest::gtest_main ${TEST_LIBRARIES})
    puls_configure_target(${name})
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

if(PULS_LIVE_TESTS)
    puls_add_test(puls_live_tests SOURCES live_test.cpp
        LIBRARIES Puls::yandex Puls::speedtestru)
endif()
if(PULS_SYSTEM_TLS_TESTS)
    puls_add_test(puls_system_tls_tests SOURCES system_tls_test.cpp LIBRARIES Puls::net)
endif()
