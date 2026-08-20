# Ordered same-instance recovery: malformed_failed then valid_after_malformed.
if(NOT CMD)
    message(FATAL_ERROR "CMD is required")
endif()
execute_process(
    COMMAND ${CMD}
    RESULT_VARIABLE rv
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
)
if(NOT rv EQUAL 0)
    message(FATAL_ERROR "scan recover exited ${rv}:\n${out}${err}")
endif()
string(FIND "${out}" "malformed_failed" i1)
string(FIND "${out}" "valid_after_malformed" i2)
if(i1 LESS 0 OR i2 LESS 0 OR i2 LESS i1)
    message(FATAL_ERROR
        "scan recover missing ordered markers:\n${out}${err}")
endif()
