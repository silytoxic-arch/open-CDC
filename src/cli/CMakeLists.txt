add_executable(opencdc main.cpp)
target_link_libraries(opencdc PRIVATE opencdc_engine opencdc_schematic CLI11::CLI11)
opencdc_set_warnings(opencdc)
