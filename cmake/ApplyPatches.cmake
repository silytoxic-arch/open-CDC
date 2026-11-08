# Applies OpenCDC's patches to a fetched dependency (third_party/patches/<prefix>-*.patch).
#
# Idempotent: a patch that is already applied is skipped, so re-configuring, an
# existing build directory and an offline mirror (FETCHCONTENT_SOURCE_DIR_<NAME>)
# all work. A patch that neither applies nor is applied stops the configuration:
# the dependency's version no longer matches the patch.
find_package(Git REQUIRED)

function(opencdc_apply_patches source_dir prefix)
    file(GLOB patches "${PROJECT_SOURCE_DIR}/third_party/patches/${prefix}-*.patch")
    list(SORT patches)
    foreach(patch IN LISTS patches)
        get_filename_component(name "${patch}" NAME)
        execute_process(COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace --reverse --check "${patch}"
                        WORKING_DIRECTORY "${source_dir}" RESULT_VARIABLE already OUTPUT_QUIET ERROR_QUIET)
        if(already EQUAL 0)
            continue()
        endif()
        execute_process(COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace "${patch}"
                        WORKING_DIRECTORY "${source_dir}" RESULT_VARIABLE rc ERROR_VARIABLE err)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "cannot apply ${name} to ${source_dir}:\n${err}")
        endif()
        message(STATUS "Applied ${name}")
    endforeach()
endfunction()
