add_executable(opencdc main.cpp)
target_link_libraries(opencdc PRIVATE opencdc_engine opencdc_schematic CLI11::CLI11 nlohmann_json::nlohmann_json)
opencdc_set_warnings(opencdc)
