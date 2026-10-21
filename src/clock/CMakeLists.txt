add_library(opencdc_clock STATIC
    ClockAnalyzer.cpp
    ClockAnalyzer.h
    ClockModel.cpp
    ClockModel.h)

target_link_libraries(opencdc_clock PUBLIC opencdc_core opencdc_graph opencdc_constraints)
opencdc_set_warnings(opencdc_clock)
