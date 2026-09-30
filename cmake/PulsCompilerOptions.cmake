# Shared compiler configuration for every Puls target.

add_library(puls_options INTERFACE)
add_library(Puls::options ALIAS puls_options)

target_compile_features(puls_options INTERFACE cxx_std_20)
# Reject Asio facilities removed from newer Boost releases so that local
# builds against distribution Boost stay compatible with the vcpkg baseline.
target_compile_definitions(puls_options INTERFACE BOOST_ASIO_NO_DEPRECATED)

if(WIN32)
    # Windows 10 is the minimum supported desktop version. UTF-8 system
    # messages keep error text valid for JSON and console output. The CRT
    # "secure" replacements for getenv and strerror are not portable.
    target_compile_definitions(puls_options INTERFACE
        _WIN32_WINNT=0x0A00
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        BOOST_SYSTEM_USE_UTF8
        _CRT_SECURE_NO_WARNINGS)
endif()

if(MSVC)
    target_compile_options(puls_options INTERFACE
        /utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor /EHsc /bigobj /W4)
    if(PULS_WARNINGS_AS_ERRORS)
        target_compile_options(puls_options INTERFACE /WX)
    endif()
else()
    target_compile_options(puls_options INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
        -Wcast-align -Wformat=2 -Wimplicit-fallthrough -Wmissing-declarations)
    if(PULS_WARNINGS_AS_ERRORS)
        target_compile_options(puls_options INTERFACE -Werror)
    endif()
endif()

if(PULS_SANITIZERS)
    if(MSVC)
        if("address" IN_LIST PULS_SANITIZERS)
            target_compile_options(puls_options INTERFACE /fsanitize=address)
        endif()
    else()
        list(JOIN PULS_SANITIZERS "," puls_sanitizer_list)
        target_compile_options(puls_options INTERFACE
            -fsanitize=${puls_sanitizer_list} -fno-omit-frame-pointer -fno-sanitize-recover=all)
        target_link_options(puls_options INTERFACE -fsanitize=${puls_sanitizer_list})
    endif()
endif()

# Applies the shared options to a first-party target.
function(puls_configure_target target)
    target_link_libraries(${target} PRIVATE Puls::options)
endfunction()
