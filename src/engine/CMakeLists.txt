add_library(opencdc_engine STATIC
    AnalysisSession.cpp
    AnalysisSession.h
    ProjectConfig.cpp
    ProjectConfig.h
    Report.cpp
    Report.h
    ResultJson.cpp
    ResultJson.h
    WaiverFile.cpp
    WaiverFile.h)

target_link_libraries(opencdc_engine
    PUBLIC  opencdc_core opencdc_ir opencdc_frontend opencdc_graph opencdc_constraints opencdc_clock opencdc_cdc opencdc_rules opencdc_schematic
    PRIVATE nlohmann_json::nlohmann_json)
opencdc_set_warnings(opencdc_engine)
