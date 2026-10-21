add_library(opencdc_schematic STATIC
    Layout.cpp
    Svg.cpp
    Schematic.cpp
    Schematic.h)

target_link_libraries(opencdc_schematic PUBLIC opencdc_cdc)
opencdc_set_warnings(opencdc_schematic)
