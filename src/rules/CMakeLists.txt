add_library(opencdc_rules STATIC
    RuleEngine.cpp
    RuleEngine.h
    Waiver.cpp
    Waiver.h)

target_link_libraries(opencdc_rules PUBLIC opencdc_cdc)
opencdc_set_warnings(opencdc_rules)
