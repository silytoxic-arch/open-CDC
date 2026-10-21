cmake_minimum_required(VERSION 3.25)

project(OpenCDC
    VERSION 0.8.0
    DESCRIPTION "OpenCDC Analyzer - FPGA clock domain crossing static analysis"
    LANGUAGES CXX)

# CMake >= 3.31: normalize install() destinations ("./docs" -> "docs").
if(POLICY CMP0177)
    cmake_policy(SET CMP0177 NEW)
endif()

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not allowed. Use a separate build directory or CMake presets.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(OPENCDC_BUILD_GUI "Build the Qt6 Widgets GUI" ON)
option(OPENCDC_BUILD_TESTS "Build unit tests (Catch2 + CTest)" ON)
option(OPENCDC_WARNINGS_AS_ERRORS "Treat warnings in OpenCDC targets as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

# All executables land in <build>/bin so tests and the GUI find each other easily.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")

include(GNUInstallDirs)
if(NOT WIN32)
    # Keep the run path to non-system libraries (e.g. a Qt outside /usr) in
    # installed binaries; system directories are left out automatically.
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)
endif()
set(OPENCDC_COMPONENT OpenCDC) # install component of everything OpenCDC ships
if(WIN32)
    set(OPENCDC_DATA_DIR ".") # flat: bin/, docs/, examples/, tools/
else()
    set(OPENCDC_DATA_DIR "${CMAKE_INSTALL_DATADIR}/opencdc")
endif()

include(CompilerWarnings)
include(Dependencies)
include(NoQtInCore)

add_subdirectory(src/core)
add_subdirectory(src/ir)
add_subdirectory(src/graph)
add_subdirectory(src/constraints)
add_subdirectory(src/clock)
add_subdirectory(src/cdc)
add_subdirectory(src/rules)
add_subdirectory(src/schematic)
add_subdirectory(src/frontend)
add_subdirectory(src/engine)
add_subdirectory(src/cli)

if(OPENCDC_BUILD_GUI)
    # Found here (not only in src/gui) so that the imported Qt targets are
    # visible to tests/ as well (the Windows GUI tests put Qt's bin/ on PATH).
    find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Concurrent)
    add_subdirectory(src/gui)
endif()

include(Packaging)

if(OPENCDC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Architectural rule: analysis libraries and the CLI must never depend on Qt.
opencdc_assert_no_qt(opencdc_core opencdc_vhdl opencdc_ir opencdc_graph opencdc_constraints opencdc_clock opencdc_cdc opencdc_rules opencdc_schematic opencdc_frontend opencdc_engine opencdc)
