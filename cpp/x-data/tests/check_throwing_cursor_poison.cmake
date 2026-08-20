# Two-phase poison gate. A missing include dir used to make the negative
# compile fail for the wrong reason and still exit 0.
if(NOT COMPILER OR NOT PROBE OR NOT INC0)
    message(FATAL_ERROR "COMPILER, PROBE, and INC0 are required")
endif()
set(INC_LIST -I${INC0})
if(INC1)
    list(APPEND INC_LIST -I${INC1})
endif()
set(FLAG_LIST)
if(FLAGS)
    separate_arguments(FLAG_LIST NATIVE_COMMAND "${FLAGS}")
endif()
if(SYSROOT)
    list(APPEND FLAG_LIST -isysroot "${SYSROOT}")
endif()
if(NOT OUT)
    set(OUT "${CMAKE_CURRENT_BINARY_DIR}/throwing_cursor_poison_probe.o")
endif()

# Positive control: same compiler, flags, includes, probe — no poison.
execute_process(
    COMMAND ${COMPILER} -std=c++23 ${FLAG_LIST} -c
        ${INC_LIST}
        ${PROBE}
        -o ${OUT}.ok.o
    RESULT_VARIABLE rv_ok
    ERROR_VARIABLE err_ok
    OUTPUT_VARIABLE out_ok
)
if(NOT rv_ok EQUAL 0)
    message(FATAL_ERROR
        "positive compile control failed (toolchain/includes broken):\n${out_ok}${err_ok}")
endif()

# Negative: poison must make the reached throwing facade fail to compile.
execute_process(
    COMMAND ${COMPILER} -std=c++23 ${FLAG_LIST} -c
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
set(combined "${out}${err}")
if(NOT combined MATCHES "peek_u8")
    message(FATAL_ERROR
        "poison compile failed for the wrong reason (expected peek_u8):\n${combined}")
endif()
message(STATUS "throwing-facade poison fired as required")
