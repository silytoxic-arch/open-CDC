add_subdirectory(vhdl)

add_library(opencdc_frontend STATIC
    Frontend.h
    slang_adapter/IrBuilder.cpp
    slang_adapter/IrBuilder.h
    slang_adapter/ProcessLowering.cpp
    slang_adapter/SlangFrontend.cpp)

target_link_libraries(opencdc_frontend
    PUBLIC  opencdc_core opencdc_ir
    PRIVATE slang::slang)   # slang stays private: no slang types in public headers
opencdc_set_warnings(opencdc_frontend)
