if(NOT DEFINED PULS_ICON_INPUT OR NOT DEFINED PULS_ICON_OUTPUT OR
        NOT DEFINED PULS_SIPS OR NOT DEFINED PULS_ICONUTIL)
    message(FATAL_ERROR "Puls icon generation requires input, output and native Apple tools")
endif()
set(iconset "${PULS_ICON_OUTPUT}.iconset")
file(MAKE_DIRECTORY "${iconset}")
foreach(size IN ITEMS 16 32 128 256 512)
    math(EXPR retina "${size} * 2")
    execute_process(COMMAND "${PULS_SIPS}" -z "${size}" "${size}" "${PULS_ICON_INPUT}"
        --out "${iconset}/icon_${size}x${size}.png"
        OUTPUT_QUIET COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${PULS_SIPS}" -z "${retina}" "${retina}" "${PULS_ICON_INPUT}"
        --out "${iconset}/icon_${size}x${size}@2x.png"
        OUTPUT_QUIET COMMAND_ERROR_IS_FATAL ANY)
endforeach()
execute_process(COMMAND "${PULS_ICONUTIL}" -c icns -o "${PULS_ICON_OUTPUT}" "${iconset}"
    COMMAND_ERROR_IS_FATAL ANY)
