# VHDL frontend (own implementation; no slang, no Qt).
add_library(opencdc_vhdl STATIC
    VhdlAst.h
    VhdlLexer.cpp
    VhdlLexer.h
    VhdlParser.cpp
    VhdlParser.h)

target_link_libraries(opencdc_vhdl PUBLIC opencdc_core)
target_include_directories(opencdc_vhdl PUBLIC ${PROJECT_SOURCE_DIR}/src)
opencdc_set_warnings(opencdc_vhdl)
