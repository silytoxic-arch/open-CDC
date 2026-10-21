add_library(opencdc_ir STATIC
    BodyEditor.cpp
    BodyEditor.h
    Const.cpp
    Const.h
    ConstFold.cpp
    ConstFold.h
    Design.cpp
    Design.h
    Dump.cpp
    Dump.h
    SigSpec.cpp
    SigSpec.h)

target_link_libraries(opencdc_ir PUBLIC opencdc_core)
opencdc_set_warnings(opencdc_ir)
