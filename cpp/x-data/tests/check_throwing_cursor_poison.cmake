# Expect compile failure of throwing_cursor_poison_probe.cpp with the poison define.
if(NOT COMPILER OR NOT PROBE OR NOT INC0)
    message(FATAL_ERROR "COMPILER, PROBE, and INC0 are required")
endif()
set(INC_LIST -I${INC0})
if(INC1)
    list(APPEND INC_LIST -I${INC1})
endif()
if(NOT OUT)
    set(OUT "${CMAKE_CURRENT_BINARY_DIR}/throwing_cursor_poison_probe.o")
endif()
execute_process(
    COMMAND ${COMPILER} -std=c++23 -c
        -DCATL_XDATA_NO_THROWING_CURSOR
        ${INC_LIST}
        ${PROBE}
        -o ${OUT}
    RESULT_VARIABLE rv
    ERROR_VARIABLE err
    OUTPUT_VARIABLE out
)
if(rv EQUAL 0)
    message(FATAL_ERROR
        "throwing-facade poison did not fire; probe compiled:\n${out}${err}")
endif()
message(STATUS "throwing-facade poison fired as required")
