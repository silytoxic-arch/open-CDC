add_library(opencdc_engine STATIC
    AnalysisSession.cpp
    AnalysisSession.h
    ProjectConfig.cpp
    ProjectConfig.h
    Report.cpp
    Report.h
    ResultJson.cpp
    ResultJson.h
    SdcSuggest.cpp
    SdcSuggest.h
    ClockNaming.h
    ClockPairs.cpp
    ClockPairs.h
    VivadoClocks.cpp
    VivadoClocks.h
    QuartusProject.cpp
    QuartusProject.h
    VivadoProject.cpp
    VivadoProject.h
    WaiverFile.cpp
    WaiverFile.h
    Xml.cpp
    Xml.h
    DiamondProject.cpp DiamondProject.h GowinProject.cpp
    GowinProject.h)

target_link_libraries(opencdc_engine
    PUBLIC  opencdc_core opencdc_ir opencdc_frontend opencdc_graph opencdc_constraints opencdc_clock opencdc_cdc opencdc_rules opencdc_schematic
    PRIVATE nlohmann_json::nlohmann_json)
opencdc_set_warnings(opencdc_engine)
