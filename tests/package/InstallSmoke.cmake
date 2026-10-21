# Installs the OpenCDC component into PREFIX and runs the installed
# programs: the installed tree must work on its own (Qt deployed on Windows).
#   -DBUILD_DIR -DCONFIG -DPREFIX -DDATA_DIR -DVERSION -DGUI=ON|OFF
file(REMOVE_RECURSE "${PREFIX}")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --config "${CONFIG}" --prefix "${PREFIX}"
                        --component OpenCDC
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "cmake --install failed (${rc}):\n${out}${err}")
endif()

set(exe "")
if(WIN32)
    set(exe ".exe")
endif()
set(cli "${PREFIX}/bin/opencdc${exe}")
set(example "${PREFIX}/${DATA_DIR}/examples/cdc_advanced/cdc_advanced.ocdc.json")
foreach(f "${cli}" "${example}" "${PREFIX}/${DATA_DIR}/docs/rules/CDC-001.md"
          "${PREFIX}/${DATA_DIR}/THIRD_PARTY_NOTICES.md" "${PREFIX}/${DATA_DIR}/tools/bench/gen_bench.py")
    if(NOT EXISTS "${f}")
        message(FATAL_ERROR "not installed: ${f}")
    endif()
endforeach()

execute_process(COMMAND "${cli}" --version RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "opencdc ${VERSION}")
    message(FATAL_ERROR "installed CLI --version: rc=${rc} '${out}${err}'")
endif()
execute_process(COMMAND "${cli}" analyze "${example}" -q RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 3 OR NOT out MATCHES "CDC     : 16 crossing path")
    message(FATAL_ERROR "installed CLI on the installed example: rc=${rc}\n${out}${err}")
endif()

if(GUI)
    set(gui "${PREFIX}/bin/opencdc-gui${exe}")
    if(WIN32)
        # windeployqt must have put the Qt runtime next to the executable.
        foreach(f Qt6Core.dll Qt6Widgets.dll platforms/qwindows.dll)
            if(NOT EXISTS "${PREFIX}/bin/${f}")
                message(FATAL_ERROR "Qt runtime not deployed: bin/${f}")
            endif()
        endforeach()
    endif()
    set(ENV{QT_QPA_PLATFORM} offscreen)
    execute_process(COMMAND "${gui}" --smoke-test "${example}" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0 OR NOT out MATCHES "GUI smoke test passed")
        message(FATAL_ERROR "installed GUI smoke test: rc=${rc}\n${out}${err}")
    endif()
endif()
message("installed tree OK: ${PREFIX}")
