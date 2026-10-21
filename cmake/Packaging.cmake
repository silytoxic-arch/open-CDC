# Install rules and CPack packaging.
#
#   cmake --build <build> --config Release
#   cmake --install <build> --config Release --prefix <dir>   # installed tree
#   cpack --config <build>/CPackConfig.cmake -C Release        # packages
#
# Windows: ZIP (always) and an NSIS installer (when makensis is on PATH).
# The Qt runtime is deployed next to opencdc-gui.exe by Qt's deploy script
# (windeployqt), so the package runs on a machine without Qt.
# Linux: TGZ and DEB; Qt comes from the distribution (not bundled).
#
# Only the OpenCDC component is installed into packages; third-party
# projects pulled in by FetchContent keep their own install rules out.

# OPENCDC_COMPONENT and OPENCDC_DATA_DIR are set in the root CMakeLists.txt
# (before src/gui, which installs the GUI).

install(TARGETS opencdc
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT ${OPENCDC_COMPONENT})

# opencdc-gui is installed (with the Qt deployment) by src/gui/CMakeLists.txt.

if(MSVC)
    # The Visual C++ runtime (vcruntime140*.dll, msvcp140*.dll) next to the
    # executables: the package must run on a machine without the VC++
    # Redistributable. (The UCRT is part of Windows 10 and later.)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ${CMAKE_INSTALL_BINDIR})
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT ${OPENCDC_COMPONENT})
    include(InstallRequiredSystemLibraries)
endif()

install(FILES README.md DESTINATION ${OPENCDC_DATA_DIR} COMPONENT ${OPENCDC_COMPONENT})
install(FILES third_party/README.md DESTINATION ${OPENCDC_DATA_DIR}
    RENAME THIRD_PARTY_NOTICES.md COMPONENT ${OPENCDC_COMPONENT})
install(DIRECTORY docs/ DESTINATION ${OPENCDC_DATA_DIR}/docs COMPONENT ${OPENCDC_COMPONENT}
    PATTERN "progress" EXCLUDE)
install(DIRECTORY examples/ DESTINATION ${OPENCDC_DATA_DIR}/examples COMPONENT ${OPENCDC_COMPONENT})
install(PROGRAMS tools/bench/gen_bench.py DESTINATION ${OPENCDC_DATA_DIR}/tools/bench
    COMPONENT ${OPENCDC_COMPONENT})

# ---- CPack ----
set(CPACK_PACKAGE_NAME "OpenCDC")
set(CPACK_PACKAGE_VENDOR "OpenCDC")
set(CPACK_PACKAGE_CONTACT "OpenCDC maintainers")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "OpenCDC Analyzer")
set(CPACK_PACKAGE_EXECUTABLES "opencdc-gui" "OpenCDC Analyzer")
set(CPACK_RESOURCE_FILE_README "${PROJECT_SOURCE_DIR}/README.md")
set(CPACK_STRIP_FILES ON)
set(CPACK_SOURCE_GENERATOR "")
set(CPACK_COMPONENTS_ALL ${OPENCDC_COMPONENT})
# One package containing the OpenCDC component only (not one per component).
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY ON) # archives unpack into OpenCDC-<version>-<system>/
set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
set(CPACK_NSIS_COMPONENT_INSTALL OFF)
set(CPACK_DEB_COMPONENT_INSTALL ON)

if(WIN32)
    set(CPACK_SYSTEM_NAME "windows-x64")
    set(CPACK_GENERATOR "ZIP")
    find_program(OPENCDC_MAKENSIS makensis)
    if(OPENCDC_MAKENSIS)
        list(APPEND CPACK_GENERATOR "NSIS")
    endif()
    set(CPACK_NSIS_DISPLAY_NAME "OpenCDC Analyzer ${PROJECT_VERSION}")
    set(CPACK_NSIS_PACKAGE_NAME "OpenCDC Analyzer")
    set(CPACK_NSIS_MODIFY_PATH ON) # optional: add bin/ to PATH for the CLI
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\opencdc-gui.exe")
else()
    set(CPACK_GENERATOR "TGZ")
    find_program(OPENCDC_DPKG dpkg-deb)
    if(OPENCDC_DPKG)
        list(APPEND CPACK_GENERATOR "DEB")
    endif()
    set(CPACK_DEBIAN_PACKAGE_NAME "opencdc")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_SECTION "electronics")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
    if(TARGET opencdc-gui)
        set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6widgets6 (>= 6.5)")
    endif()
endif()

include(CPack)
