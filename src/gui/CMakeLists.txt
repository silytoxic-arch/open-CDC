find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Concurrent)
qt_standard_project_setup()

qt_add_executable(opencdc-gui
    main.cpp
    AnalysisController.cpp
    AnalysisController.h
    CdcViews.cpp
    CdcViews.h
    ClockView.cpp
    ClockView.h
    MainWindow.cpp
    MainWindow.h
    ProjectEditor.cpp
    ProjectEditor.h
    SchematicView.cpp
    SchematicView.h
    SourceViewer.cpp
    SourceViewer.h
    WaiverDialog.cpp
    WaiverDialog.h)

target_link_libraries(opencdc-gui PRIVATE
    opencdc_engine
    opencdc_schematic
    Qt6::Widgets
    Qt6::Concurrent)
opencdc_set_warnings(opencdc-gui)

set_target_properties(opencdc-gui PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON)
if(APPLE)
    # OpenCDC.app (Contents/MacOS/OpenCDC); the CLI stays bin/opencdc
    set_target_properties(opencdc-gui PROPERTIES
        OUTPUT_NAME "OpenCDC"
        MACOSX_BUNDLE_BUNDLE_NAME "OpenCDC Analyzer"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.opencdc.analyzer"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_INFO_STRING "OpenCDC Analyzer ${PROJECT_VERSION}"
        MACOSX_BUNDLE_COPYRIGHT "OpenCDC maintainers")
endif()

install(TARGETS opencdc-gui
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT ${OPENCDC_COMPONENT}
    BUNDLE DESTINATION . COMPONENT ${OPENCDC_COMPONENT})
if(WIN32 OR APPLE)
    # Runs windeployqt / macdeployqt at install time, so that the installed
    # tree and the packages work without a Qt installation.
    qt_generate_deploy_app_script(
        TARGET opencdc-gui
        OUTPUT_SCRIPT _opencdc_deploy_script
        NO_TRANSLATIONS
        NO_UNSUPPORTED_PLATFORM_ERROR)
    install(SCRIPT ${_opencdc_deploy_script} COMPONENT ${OPENCDC_COMPONENT})
    if(APPLE)
        install(SCRIPT "${PROJECT_SOURCE_DIR}/cmake/MacDeployPrune.cmake" COMPONENT ${OPENCDC_COMPONENT})
    endif()
endif()
