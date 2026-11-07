# opencdc import end to end, on a copy of the Vivado project example (the
# example itself is never written): the project is written next to the .xpr,
# an existing project is not replaced without --force, the written project
# analyzes as the .xpr does, and a Vivado project is refused where an OpenCDC
# project is needed (waivers) or given as the output.
file(REMOVE_RECURSE "${WORK}")
file(COPY "${EXAMPLE_DIR}/" DESTINATION "${WORK}")
set(xpr "${WORK}/demo.xpr")

function(run expected_rc regex)
    execute_process(COMMAND "${EXE}" ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    set(all "${out}${err}")
    message("---- opencdc ${ARGN}: ${rc}\n${all}")
    if(NOT rc EQUAL ${expected_rc})
        message(FATAL_ERROR "exit code ${rc}, expected ${expected_rc}")
    endif()
    string(REGEX REPLACE "\r?\n" " " flat "${all}")
    if(NOT flat MATCHES "${regex}")
        message(FATAL_ERROR "output does not match: ${regex}")
    endif()
endfunction()

run(0 "PRJ-XPR-DISABLED.*Vivado project imported: 2 source\\(s\\), 2 constraint file\\(s\\), top 'demo_top'.*OpenCDC project written: .*demo.ocdc.json"
    import "${xpr}")
if(NOT EXISTS "${WORK}/demo.ocdc.json")
    message(FATAL_ERROR "demo.ocdc.json was not written next to the .xpr")
endif()
file(READ "${WORK}/demo.ocdc.json" pj)
string(REGEX REPLACE "[ \r\n]+" " " pj "${pj}")
if(NOT pj MATCHES "\"file\": \"rtl/demo_lib_sync.vhd\", \"library\": \"demo_lib\"" OR NOT pj MATCHES "\"rtl/include\""
   OR NOT pj MATCHES "\"ref\": \"demo_lib_sync\"" OR pj MATCHES "old_top" OR pj MATCHES "demo_tb")
    message(FATAL_ERROR "unexpected project:\n${pj}")
endif()
# negative: an existing project is kept
run(2 "exists \\(--force replaces it\\)" import "${xpr}")
run(0 "OpenCDC project written" import "${xpr}" --force)
# the written project analyzes as the .xpr
run(0 "CDC     : 2 crossing path\\(s\\), 1 synchronized, 1 unsynchronized" analyze "${WORK}/demo.ocdc.json" -q)
run(0 "Vivado project imported.*CDC     : 2 crossing path\\(s\\), 1 synchronized, 1 unsynchronized" analyze "${xpr}" -q)
# negatives: waivers need an OpenCDC project; the output must not be a .xpr; the input must be one
run(2 "is a Vivado project: waivers belong to an OpenCDC project" waive "${xpr}" --fingerprint 0 --rule CDC-003 --reason x)
run(2 "must be an OpenCDC project" import "${xpr}" -o "${WORK}/other.xpr")
run(2 "not a Vivado project" import "${WORK}/demo.ocdc.json" -o "${WORK}/x.ocdc.json")
