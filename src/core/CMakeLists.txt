configure_file(Version.h.in "${PROJECT_BINARY_DIR}/generated/core/Version.h" @ONLY)

add_library(opencdc_core STATIC
    Diagnostic.cpp
    Diagnostic.h
    Id.h
    PathUtil.h
    Progress.h
    Result.h
    SourceLocation.h)

target_include_directories(opencdc_core PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/generated")
target_compile_features(opencdc_core PUBLIC cxx_std_20)
opencdc_set_warnings(opencdc_core)
