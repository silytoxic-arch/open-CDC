# opencdc waivers: a project (in WORK) using the cdc_advanced example sources
# with an active, an unused, an expired and an invalid waiver.
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(src "${EXAMPLE_DIR}/rtl")
file(WRITE "${WORK}/p.ocdc.json" "{\"format\": \"opencdc-project\", \"version\": 1, \"name\": \"audit\", \"top\": \"cdc_advanced\",
 \"sources\": [\"${src}/sync_2ff.v\", \"${src}/sync_vec.v\", \"${src}/rst_sync.v\", \"${src}/pulse_sync.v\",
               \"${src}/gray_fifo.v\", \"${src}/handshake.v\", \"${src}/cdc_advanced.v\"],
 \"constraints\": [\"${EXAMPLE_DIR}/constraints/cdc_advanced.sdc\"], \"waivers\": [\"w.json\"]}\n")
file(WRITE "${WORK}/w.json" "{\"format\": \"opencdc-waivers\", \"version\": 1, \"waivers\": [
 {\"id\": \"W-OK\", \"rule\": \"CDC-007\", \"source\": \"cdc_advanced.a_kick\", \"reason\": \"level\"},
 {\"id\": \"W-STALE\", \"rule\": \"CDC-007\", \"source\": \"cdc_advanced.nosuch\", \"reason\": \"renamed signal\"},
 {\"id\": \"W-OLD\", \"rule\": \"CDC-008\", \"reason\": \"old\", \"expires\": \"2020-01-01\"},
 {\"id\": \"W-BAD\", \"rule\": \"CDC-001\"}]}\n")
set(project "${WORK}/p.ocdc.json")

function(run expected_rc)
    execute_process(COMMAND "${EXE}" waivers "${project}" ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    message("---- waivers ${ARGN} ----\n${out}${err}")
    if(NOT rc EQUAL ${expected_rc})
        message(FATAL_ERROR "exit code ${rc}, expected ${expected_rc}")
    endif()
    set(out "${out}${err}" PARENT_SCOPE)
endfunction()

run(0)
foreach(re "Waivers : 3 from 1 file\\(s\\): 1 active, 0 expiring, 1 unused, 1 expired; 1 invalid"
           "W-OK \\[active\\] uses=1 rule=CDC-007 source=cdc_advanced.a_kick"
           "W-STALE \\[unused\\] uses=0" "W-OLD \\[expired\\] uses=0 rule=CDC-008 expires=2020-01-01"
           "WAIVER-INVALID[^\n]*W-BAD" "Audit   : passed")
    if(NOT out MATCHES "${re}")
        message(FATAL_ERROR "output does not match ${re}")
    endif()
endforeach()
run(4 --fail-on unused)
if(NOT out MATCHES "Audit   : FAILED \\(unused\\)")
    message(FATAL_ERROR "no FAILED (unused)")
endif()
run(0 --fail-on expiring) # negative: nothing expires soon
run(4 --fail-on invalid,expired --json "${WORK}/audit.json")
if(NOT out MATCHES "FAILED \\(invalid, expired\\)") # in --fail-on order
    message(FATAL_ERROR "no FAILED (invalid, expired)")
endif()
file(READ "${WORK}/audit.json" j)
string(JSON n LENGTH "${j}" waivers)
string(JSON s1 GET "${j}" waivers 1 state)
string(JSON inv GET "${j}" invalid)
if(NOT n EQUAL 3 OR NOT s1 STREQUAL "unused" OR NOT inv EQUAL 1)
    message(FATAL_ERROR "audit.json: ${n} waivers, second ${s1}, invalid ${inv}")
endif()
run(2 --fail-on bogus) # negative: unknown state
message("opencdc waivers: ok")
