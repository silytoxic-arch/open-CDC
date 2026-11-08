# Third-party components

All dependencies are fetched at configure time by `cmake/Dependencies.cmake`
(CMake `FetchContent`) and pinned to exact versions. Nothing is vendored in
this directory; `patches/` holds OpenCDC's fixes to them (see below).

| Component | Version | License | Used by | How it is obtained |
|---|---|---|---|---|
| [slang](https://github.com/MikePopoloski/slang) | v12.0 | MIT | `opencdc_frontend` (Verilog/SV preprocess, parse, elaborate) | FetchContent (git tag) |
| ├─ [fmt](https://github.com/fmtlib/fmt) | 12.2.0 (pinned by slang) | MIT | slang | fetched by slang |
| ├─ boost (unordered, regex subsets) | 1.91 (pinned by slang) | BSL-1.0 | slang | bundled / fetched by slang |
| ├─ [toml++](https://github.com/marzer/tomlplusplus) | v3.4.0 (pinned by slang) | MIT | slang | fetched by slang |
| [nlohmann/json](https://github.com/nlohmann/json) | v3.12.0 | MIT | `opencdc_engine` (project files, JSON result) | FetchContent (release tarball, SHA-256 pinned) |
| [CLI11](https://github.com/CLIUtils/CLI11) | v2.5.0 | BSD-3-Clause | `opencdc` CLI | FetchContent (git tag) |
| [Catch2](https://github.com/catchorg/Catch2) | v3.16.0 | BSL-1.0 | unit tests only | FetchContent (git tag) |
| [Qt](https://www.qt.io/) | ≥ 6.5 (CI: 6.8.3) | LGPL-3.0 (open source edition) | `opencdc-gui` only | installed separately, found with `find_package(Qt6)` |

## Patches

`third_party/patches/<dependency>-NNNN-*.patch` are OpenCDC's fixes to a
dependency, applied by `cmake/ApplyPatches.cmake` right after the sources are
fetched (and to an offline mirror given with `FETCHCONTENT_SOURCE_DIR_<NAME>`).
Applying is idempotent; a patch that no longer applies stops the configuration,
so a version update cannot silently drop a fix.

| Patch | Fixes |
|---|---|
| `slang-0001-implicit-parameter-type-from-defparam.patch` | An implicitly typed parameter without a default value whose value is set by a `defparam` (`parameter [9:0] P;`, `parameter P;`, `parameter signed P;`) got the error type, so every reference to it evaluated as invalid without a diagnostic (registers that depend on it were lost). It now takes the declared range and signing, or else the type of the value (IEEE 1800-2017 6.20.2). Found in MiSTer-devel/NES_MiSTer. |

## Licensing notes

* Qt is linked **dynamically** and only by the GUI target, which keeps the
  LGPL-3.0 obligations satisfiable for both open-source and closed-source
  distribution (ship Qt as replaceable DLLs, provide the Qt license text).
* No GPL-licensed component is linked into OpenCDC. Any future VHDL frontend
  must keep this property (see docs/architecture.md §2.3).
* The analysis libraries (`opencdc_core`, `opencdc_frontend`, `opencdc_engine`)
  and the CLI are checked at configure time to not depend on Qt
  (`cmake/NoQtInCore.cmake`).

## Building offline

Clone each repository at the pinned tag and pass, for example:

```
cmake --preset windows-msvc -DFETCHCONTENT_SOURCE_DIR_SLANG=D:/mirror/slang ^
      -DFETCHCONTENT_SOURCE_DIR_CLI11=D:/mirror/CLI11 ...
```
