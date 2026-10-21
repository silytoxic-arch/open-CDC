add_library(opencdc_graph STATIC
    DesignView.cpp
    DesignView.h)

target_link_libraries(opencdc_graph PUBLIC opencdc_ir)
opencdc_set_warnings(opencdc_graph)
