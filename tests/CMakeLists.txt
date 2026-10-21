include(Catch)

add_executable(opencdc_tests
    TestUtil.h
    core/test_diagnostic.cpp
    core/test_id_result.cpp
    ir/test_ir.cpp
    ir_extract/test_ir_extract.cpp
    graph/test_design_view.cpp
    constraints/test_sdc.cpp
    clock/test_clock.cpp
    cdc/CdcTestUtil.h
    cdc/test_cdc.cpp
    cdc/test_cdc_advanced.cpp
    schematic/test_schematic.cpp
    DesignUtil.h
    engine/test_project_config.cpp
    engine/test_analysis_session.cpp
    engine/test_waivers.cpp
    engine/test_report.cpp
    frontend/test_slang_frontend.cpp
    vhdl/test_vhdl_parser.cpp
    vhdl/test_vhdl_elab.cpp
    vhdl/test_vhdl_mixed.cpp)

target_link_libraries(opencdc_tests PRIVATE
    opencdc_vhdl
    opencdc_engine
    opencdc_ir
    opencdc_clock
    opencdc_rules
    opencdc_schematic
    nlohmann_json::nlohmann_json
    Catch2::Catch2WithMain)
target_compile_definitions(opencdc_tests PRIVATE
    OPENCDC_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples"
    OPENCDC_TESTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
opencdc_set_warnings(opencdc_tests)

catch_discover_tests(opencdc_tests
    TEST_PREFIX "unit."
    DISCOVERY_MODE PRE_TEST)

# ---- CLI end-to-end tests (check exit code AND output) ----
set(_smoke_project "${PROJECT_SOURCE_DIR}/examples/smoke/smoke.ocdc.json")
function(opencdc_cli_test name expected_rc expected_regex)
    add_test(NAME cli.${name}
        COMMAND ${CMAKE_COMMAND}
            "-DEXE=$<TARGET_FILE:opencdc>"
            "-DARGS=${ARGN}"
            "-DEXPECTED_RC=${expected_rc}"
            "-DEXPECTED_REGEX=${expected_regex}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cli/RunCli.cmake")
endfunction()

opencdc_cli_test(version 0 "opencdc ${PROJECT_VERSION}" --version)
opencdc_cli_test(analyze_smoke 0 "Top     : top.*Instances: 8.*CDC     : 7 crossing path.*Rules   : 0 error violation.*1 path\\(s\\) at potential risk, 4 path\\(s\\) need review"
    analyze ${_smoke_project})
opencdc_cli_test(analyze_json 0 "JSON    : "
    analyze ${_smoke_project} --quiet --json ${CMAKE_CURRENT_BINARY_DIR}/smoke_result.json)
opencdc_cli_test(analyze_cdc_rules 3
    "CDC     : 12 crossing path.*Rules   : 5 error violation.*CDC-001.*CDC-004.*CDC-010.*Synchronizer structures:.*toggle"
    analyze ${PROJECT_SOURCE_DIR}/examples/cdc_rules/cdc_rules.ocdc.json)
opencdc_cli_test(missing_project 2 "PRJ-OPEN"
    analyze ${CMAKE_CURRENT_BINARY_DIR}/does_not_exist.ocdc.json)
opencdc_cli_test(analyze_cdc_advanced 3
    "CDC     : 16 crossing path.*11 synchronizer structure.*CDC-006.*async-fifo.*CDC-005.*CDC-007.*CDC-008.*CDC-004.*CDC-009.*S0 async-fifo.*S1 handshake"
    analyze ${PROJECT_SOURCE_DIR}/examples/cdc_advanced/cdc_advanced.ocdc.json)
opencdc_cli_test(analyze_schematics 3 "Schematics: 16 SVG file\\(s\\)"
    analyze ${PROJECT_SOURCE_DIR}/examples/cdc_advanced/cdc_advanced.ocdc.json -q
    --schematic-dir ${CMAKE_CURRENT_BINARY_DIR}/schematics)
opencdc_cli_test(analyze_waived 3 "Waiver application \\([0-9.]+ s\\) - 2 waiver\\(s\\).*3 rule hit\\(s\\) waived.*waived by W-CFG"
    analyze ${PROJECT_SOURCE_DIR}/examples/cdc_advanced/cdc_advanced_waived.ocdc.json)
opencdc_cli_test(analyze_reports 3 "HTML    : .*report.html.*CSV     : .*paths.csv, violations.csv"
    analyze ${PROJECT_SOURCE_DIR}/examples/cdc_advanced/cdc_advanced_waived.ocdc.json -q
    --html ${CMAKE_CURRENT_BINARY_DIR}/report/report.html --csv ${CMAKE_CURRENT_BINARY_DIR}/report)
opencdc_cli_test(no_subcommand 2 "")

# ---- Installed tree (install rules, Qt deployment on Windows) ----
add_test(NAME package.install_smoke
    COMMAND ${CMAKE_COMMAND}
        "-DBUILD_DIR=${PROJECT_BINARY_DIR}"
        "-DCONFIG=$<CONFIG>"
        "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/install_smoke/$<CONFIG>"
        "-DDATA_DIR=${OPENCDC_DATA_DIR}"
        "-DVERSION=${PROJECT_VERSION}"
        "-DGUI=$<TARGET_EXISTS:opencdc-gui>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/package/InstallSmoke.cmake")
set_tests_properties(package.install_smoke PROPERTIES LABELS package)

# ---- Benchmark regression (generated design, exact synchronizer census) ----
# tools/bench/gen_bench.py is deterministic; a 4-domain SoC with 128 blocks
# per domain has ~4.8k registers and every kind of link. Larger runs are
# documented in docs/benchmarks.md.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    set(_bench_dir "${CMAKE_CURRENT_BINARY_DIR}/bench/soc_4x128")
    add_test(NAME bench.generate_soc
        COMMAND ${Python3_EXECUTABLE} "${PROJECT_SOURCE_DIR}/tools/bench/gen_bench.py"
            soc --domains 4 --blocks 128 --out "${_bench_dir}")
    set_tests_properties(bench.generate_soc PROPERTIES FIXTURES_SETUP bench_soc LABELS bench)
    opencdc_cli_test(bench_soc 3
        "4788 registers.*784 crossing path.*748 synchronizer structure\\(s\\) \\(async-fifo x18, qualified-data x360, reset-sync x4, single-flop x6, toggle x360\\); 778 synchronized and 6 unsynchronized.*hits: CDC-003 x6"
        analyze "${_bench_dir}/bench.ocdc.json" -q)
    set_tests_properties(cli.bench_soc PROPERTIES FIXTURES_REQUIRED bench_soc LABELS bench)
endif()

# ---- GUI smoke test (headless, offscreen platform) ----
if(TARGET opencdc-gui)
    add_test(NAME gui.smoke
        COMMAND opencdc-gui --smoke-test ${_smoke_project})
    set_tests_properties(gui.smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME gui.cdc_rules
        COMMAND opencdc-gui --smoke-test ${PROJECT_SOURCE_DIR}/examples/cdc_rules/cdc_rules.ocdc.json)
    set_tests_properties(gui.cdc_rules PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME gui.cdc_advanced
        COMMAND opencdc-gui --smoke-test ${PROJECT_SOURCE_DIR}/examples/cdc_advanced/cdc_advanced.ocdc.json)
    set_tests_properties(gui.cdc_advanced PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME gui.cdc_advanced_waived
        COMMAND opencdc-gui --smoke-test ${PROJECT_SOURCE_DIR}/examples/cdc_advanced/cdc_advanced_waived.ocdc.json)
    set_tests_properties(gui.cdc_advanced_waived PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(WIN32)
        # Let the test find the Qt DLLs (and, through them, the platform plugins)
        # without running windeployqt first.
        set_tests_properties(gui.smoke gui.cdc_rules gui.cdc_advanced gui.cdc_advanced_waived PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
endif()

# Development tool: batch-parse VHDL files (used for the corpus check).
add_executable(opencdc-vhdl-parse ${PROJECT_SOURCE_DIR}/tools/vhdl/vhdl_parse.cpp)
target_link_libraries(opencdc-vhdl-parse PRIVATE opencdc_vhdl)
opencdc_set_warnings(opencdc-vhdl-parse)
add_executable(opencdc-vhdl-elab ${PROJECT_SOURCE_DIR}/tools/vhdl/vhdl_elab.cpp)
target_link_libraries(opencdc-vhdl-elab PRIVATE opencdc_frontend)
opencdc_set_warnings(opencdc-vhdl-elab)
