# Runs at install time on macOS, after Qt's deploy script (macdeployqt) filled
# OpenCDC.app. macdeployqt deploys every plugin of the Qt installation for the
# modules in use: with Homebrew's Qt that includes the virtual keyboard input
# context (which pulls in QtQuick and QtQml) and image format plugins whose Qt
# frameworks are not installed (QtPdf, QtSvg). OpenCDC needs the Cocoa platform
# plugin, the macOS style and a few image formats. This script keeps those
# plugins, removes the frameworks and libraries that nothing kept refers to,
# clears extended attributes (they would become ._ files in the archives) and
# signs the bundle again (ad hoc), since removing files breaks its seal.

set(prefix "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}")
set(app "${prefix}/OpenCDC.app")
if(NOT EXISTS "${app}/Contents/MacOS/OpenCDC")
    message(FATAL_ERROR "MacDeployPrune: no app bundle at ${app}")
endif()
set(contents "${app}/Contents")

set(keep_plugins
    platforms/libqcocoa.dylib
    styles/libqmacstyle.dylib
    imageformats/libqgif.dylib
    imageformats/libqico.dylib
    imageformats/libqicns.dylib
    imageformats/libqjpeg.dylib)
file(GLOB_RECURSE plugins RELATIVE "${contents}/PlugIns" "${contents}/PlugIns/*.dylib")
foreach(p IN LISTS plugins)
    if(NOT p IN_LIST keep_plugins)
        file(REMOVE "${contents}/PlugIns/${p}")
    endif()
endforeach()
file(GLOB plugin_dirs LIST_DIRECTORIES true "${contents}/PlugIns/*")
foreach(d IN LISTS plugin_dirs)
    file(GLOB left "${d}/*")
    if(IS_DIRECTORY "${d}" AND NOT left)
        file(REMOVE_RECURSE "${d}")
    endif()
endforeach()

# the frameworks and libraries reachable from the executable and the kept plugins
set(queue "${contents}/MacOS/OpenCDC")
foreach(p IN LISTS keep_plugins)
    if(EXISTS "${contents}/PlugIns/${p}")
        list(APPEND queue "${contents}/PlugIns/${p}")
    endif()
endforeach()
set(needed "")
set(seen "")
while(queue)
    list(POP_FRONT queue bin)
    if(bin IN_LIST seen)
        continue()
    endif()
    list(APPEND seen "${bin}")
    execute_process(COMMAND otool -L "${bin}" OUTPUT_VARIABLE out RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "MacDeployPrune: otool -L ${bin} failed")
    endif()
    string(REPLACE "\n" ";" lines "${out}")
    foreach(line IN LISTS lines)
        if(line MATCHES "^[ \t]+(@[a-z_]+/.*) \\(compatibility")
            set(ref "${CMAKE_MATCH_1}")
            if(ref MATCHES "([^/]+\\.framework)/")
                set(name "${CMAKE_MATCH_1}")
                string(REGEX REPLACE "\\.framework$" "" stem "${name}")
                set(target "${contents}/Frameworks/${name}/Versions/A/${stem}")
            elseif(ref MATCHES "([^/]+\\.dylib)$")
                set(name "${CMAKE_MATCH_1}")
                set(target "${contents}/Frameworks/${name}")
            else()
                continue()
            endif()
            if(EXISTS "${target}" AND NOT name IN_LIST needed)
                list(APPEND needed "${name}")
                list(APPEND queue "${target}")
            endif()
        endif()
    endforeach()
endwhile()
file(GLOB deployed RELATIVE "${contents}/Frameworks" "${contents}/Frameworks/*")
set(removed "")
foreach(f IN LISTS deployed)
    if(NOT f IN_LIST needed)
        file(REMOVE_RECURSE "${contents}/Frameworks/${f}")
        list(APPEND removed "${f}")
    endif()
endforeach()
list(LENGTH needed n_needed)
list(LENGTH removed n_removed)
message(STATUS "MacDeployPrune: kept ${n_needed} framework(s)/libraries, removed ${n_removed}: ${removed}")

execute_process(COMMAND xattr -cr "${prefix}")
execute_process(COMMAND codesign --force --deep --sign - "${app}" RESULT_VARIABLE rc ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "MacDeployPrune: codesign failed: ${err}")
endif()
