# Runs EXE with ARGS (a ;-separated list) and checks the exit code and the
# combined stdout/stderr output against EXPECTED_RC / EXPECTED_REGEX.
execute_process(COMMAND "${EXE}" ${ARGS}
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
set(all "${out}${err}")
message("---- output ----\n${all}----------------")
if(NOT rc STREQUAL "${EXPECTED_RC}")
    message(FATAL_ERROR "exit code '${rc}', expected '${EXPECTED_RC}'")
endif()
if(NOT "${EXPECTED_REGEX}" STREQUAL "")
    string(REGEX REPLACE "\r?\n" " " flat "${all}")
    if(NOT flat MATCHES "${EXPECTED_REGEX}")
        message(FATAL_ERROR "output does not match regex: ${EXPECTED_REGEX}")
    endif()
endif()
