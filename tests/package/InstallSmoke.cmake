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
        # windeployqt must have put the Qt runtime next to the executable
        # (Qt6Core.dll, or Qt6Cored.dll in Debug) and the platform plugin under
        # plugins/ (located through bin/qt.conf) or bin/.
        file(GLOB qtcore "${PREFIX}/bin/Qt6Core*.dll")
        file(GLOB qpa "${PREFIX}/plugins/platforms/qwindows*.dll" "${PREFIX}/bin/platforms/qwindows*.dll")
        if(NOT qtcore OR NOT qpa)
            message(FATAL_ERROR "Qt runtime not deployed: Qt6Core='${qtcore}' qwindows='${qpa}'")
        endif()
        # Run the installed GUI without any Qt installation on PATH, so that
        # only the deployed runtime can satisfy it.
        set(clean_path "")
        foreach(dir IN LISTS ENV{PATH})
            file(TO_CMAKE_PATH "${dir}" cdir)
            file(GLOB has_qt "${cdir}/Qt6Core*.dll")
            if(NOT has_qt)
                list(APPEND clean_path "${dir}")
            endif()
        endforeach()
        set(ENV{PATH} "${clean_path}")
        unset(ENV{QT_PLUGIN_PATH})
        unset(ENV{QT_QPA_PLATFORM_PLUGIN_PATH})
        # windeployqt ships only the qwindows platform plugin (no offscreen).
        unset(ENV{QT_QPA_PLATFORM})
    else()
        set(ENV{QT_QPA_PLATFORM} offscreen)
    endif()
    execute_process(COMMAND "${gui}" --smoke-test "${example}" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0 OR NOT out MATCHES "GUI smoke test passed")
        message(FATAL_ERROR "installed GUI smoke test: rc=${rc}\n${out}${err}")
    endif()
endif()
message("installed tree OK: ${PREFIX}")
