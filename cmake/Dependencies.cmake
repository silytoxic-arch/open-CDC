# Third-party dependencies, all pinned to exact versions.
# See third_party/README.md for licenses.
#
# To build offline, pre-populate sources and pass e.g.
#   -DFETCHCONTENT_SOURCE_DIR_SLANG=/path/to/slang
include(FetchContent)

# ---- slang (MIT): SystemVerilog/Verilog preprocessor, parser, elaborator ----
set(SLANG_INCLUDE_TOOLS OFF CACHE BOOL "" FORCE)
set(SLANG_INCLUDE_TESTS OFF CACHE BOOL "" FORCE)
set(SLANG_INCLUDE_DOCS OFF CACHE BOOL "" FORCE)
set(SLANG_INCLUDE_PYLIB OFF CACHE BOOL "" FORCE)
set(SLANG_INCLUDE_INSTALL OFF CACHE BOOL "" FORCE)
set(SLANG_INCLUDE_EXAMPLES OFF CACHE BOOL "" FORCE)
# mimalloc replaces the global allocator; keep the default allocator so the
# Qt GUI and the analysis engine share one heap.
set(SLANG_USE_MIMALLOC OFF CACHE BOOL "" FORCE)
FetchContent_Declare(slang
    GIT_REPOSITORY https://github.com/MikePopoloski/slang.git
    GIT_TAG        v12.0
    GIT_SHALLOW    ON
    SYSTEM)

# ---- nlohmann/json (MIT): project files and JSON reports ----
FetchContent_Declare(nlohmann_json
    URL      https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SYSTEM)

# ---- CLI11 (BSD-3-Clause): command line parsing ----
set(CLI11_PRECOMPILED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG        v2.5.0
    GIT_SHALLOW    ON
    SYSTEM)

FetchContent_MakeAvailable(slang nlohmann_json CLI11)

if(OPENCDC_BUILD_TESTS)
    # ---- Catch2 v3 (BSL-1.0): unit tests ----
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.16.0
        GIT_SHALLOW    ON
        SYSTEM)
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()
