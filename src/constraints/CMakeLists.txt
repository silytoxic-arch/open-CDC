add_library(opencdc_constraints STATIC
    Sdc.cpp
    Lpf.cpp
    Lpf.h
    Sdc.h)

target_link_libraries(opencdc_constraints PUBLIC opencdc_core opencdc_graph)
opencdc_set_warnings(opencdc_constraints)
