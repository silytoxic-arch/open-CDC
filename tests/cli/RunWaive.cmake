# opencdc waive end to end, on a copy of an example project (the example
# itself is never written): waive the path with fingerprint FP for RULE,
# check the waiver file is created and added to the project, the analysis
# shows the hit waived, and a duplicate id is refused.
file(REMOVE_RECURSE "${WORK}")
file(COPY "${EXAMPLE_DIR}/" DESTINATION "${WORK}")
file(REMOVE "${WORK}/waivers.json") # start without a waiver file
set(project "${WORK}/cdc_advanced.ocdc.json")

execute_process(COMMAND "${EXE}" analyze "${project}" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
string(REGEX MATCH "a_kick[^\n]*fingerprint=([0-9a-f]+)" m "${out}")
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "no fingerprint printed for the a_kick path:\n${out}")
endif()
set(fp "${CMAKE_MATCH_1}")
string(REGEX MATCH "Rules   : [^\n]*" before "${out}")

# negative: no reason
execute_process(COMMAND "${EXE}" waive "${project}" --fingerprint ${fp} --rule CDC-007
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2)
    message(FATAL_ERROR "waive without --reason: exit code ${rc}, expected 2")
endif()

execute_process(COMMAND "${EXE}" waive "${project}" --fingerprint ${fp} --rule CDC-007 --reason "level, held for many cycles"
    --author ci RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message("${out}${err}")
if(NOT rc EQUAL 0 OR NOT out MATCHES "waiver 'W-1' added to .*waivers.json.*waiver file added to project")
    message(FATAL_ERROR "waive failed: ${rc}")
endif()
if(NOT EXISTS "${WORK}/waivers.json")
    message(FATAL_ERROR "waivers.json was not created")
endif()
file(READ "${project}" pj)
if(NOT pj MATCHES "\"waivers\": *\\[[^]]*waivers.json")
    message(FATAL_ERROR "the project does not list waivers.json:\n${pj}")
endif()

execute_process(COMMAND "${EXE}" analyze "${project}" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT out MATCHES "CDC-007[^\n]*\\[waived by W-1\\]" OR NOT out MATCHES "1 hit\\(s\\) waived" OR out MATCHES "WAIVER-UNUSED")
    message(FATAL_ERROR "the hit is not waived after 'opencdc waive' (before: ${before}):\n${out}")
endif()

# negative: the same id again
execute_process(COMMAND "${EXE}" waive "${project}" --fingerprint ${fp} --rule CDC-007 --reason again --id W-1
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2 OR NOT err MATCHES "already exists")
    message(FATAL_ERROR "a duplicate waiver id was not refused: ${rc} ${err}")
endif()
message("opencdc waive: ok (fingerprint ${fp})")
