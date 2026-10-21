add_library(opencdc_cdc STATIC
    AdvancedAnalysis.cpp
    AnalyzerState.h
    CdcAnalyzer.cpp
    CdcAnalyzer.h
    CdcModel.cpp
    CdcModel.h
    SeqGraph.cpp
    SeqGraph.h)

target_link_libraries(opencdc_cdc PUBLIC opencdc_core opencdc_graph opencdc_clock opencdc_constraints)
opencdc_set_warnings(opencdc_cdc)
