# VHDL frontend (own implementation; no slang, no Qt).
add_library(opencdc_vhdl STATIC
    VhdlAst.h
    VhdlBuiltins.cpp
    VhdlElab.cpp
    VhdlElab.h
    VhdlExpr.cpp
    VhdlFrontend.cpp
    VhdlLexer.cpp
    VhdlLexer.h
    VhdlMixed.cpp
    VhdlNames.cpp
    VhdlParser.cpp
    VhdlParser.h
    VhdlScope.cpp
    VhdlStmt.cpp
    VhdlTypes.cpp
    VhdlTypes.h)

target_link_libraries(opencdc_vhdl PUBLIC opencdc_core opencdc_ir)
target_include_directories(opencdc_vhdl PUBLIC ${PROJECT_SOURCE_DIR}/src)
opencdc_set_warnings(opencdc_vhdl)
