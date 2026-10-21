add_subdirectory(vhdl)

# Vendor primitive models embedded into the binary (see Primitives.h).
set(_prim_dir ${CMAKE_CURRENT_SOURCE_DIR}/primitives)
set(_prim_files
    ${_prim_dir}/xilinx_unisim.v
    ${_prim_dir}/xilinx_xpm.v
    ${_prim_dir}/intel.v
    ${_prim_dir}/unisim_vcomponents.vhd
    ${_prim_dir}/xpm_vcomponents.vhd
    ${_prim_dir}/altera_mf_components.vhd)
set(_prim_cpp ${CMAKE_CURRENT_BINARY_DIR}/PrimitiveSources.cpp)
add_custom_command(
    OUTPUT ${_prim_cpp}
    COMMAND ${CMAKE_COMMAND} "-DOUT=${_prim_cpp}" "-DFILES=${_prim_files}"
            -P ${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake
    DEPENDS ${_prim_files} ${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake
    COMMENT "Embedding vendor primitive models"
    VERBATIM)

add_library(opencdc_frontend STATIC
    AutoFrontend.cpp
    Frontend.h
    MixedSupport.h
    Primitives.cpp
    Primitives.h
    ${_prim_cpp}
    slang_adapter/IrBuilder.cpp
    slang_adapter/IrBuilder.h
    slang_adapter/ProcessLowering.cpp
    slang_adapter/SlangFrontend.cpp)

target_link_libraries(opencdc_frontend
    PUBLIC  opencdc_core opencdc_ir opencdc_vhdl
    PRIVATE slang::slang)   # slang stays private: no slang types in public headers
opencdc_set_warnings(opencdc_frontend)
