configure_file(Version.h.in "${PROJECT_BINARY_DIR}/generated/core/Version.h" @ONLY)

add_library(opencdc_core STATIC
    Diagnostic.cpp
    Diagnostic.h
    Id.h
    PathUtil.h
    Progress.h
    Result.h
    SourceLocation.h
    TextEncoding.cpp
    TextEncoding.h
    Zip.cpp
    Zip.h)

target_include_directories(opencdc_core PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/generated")
target_compile_features(opencdc_core PUBLIC cxx_std_20)
opencdc_set_warnings(opencdc_core)
if(APPLE)
    find_library(OPENCDC_ICONV iconv)
    if(OPENCDC_ICONV)
        target_link_libraries(opencdc_core PUBLIC ${OPENCDC_ICONV})
    endif()
endif()
