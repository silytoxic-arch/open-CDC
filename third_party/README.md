# Third-party components

All dependencies are fetched at configure time by `cmake/Dependencies.cmake`
(CMake `FetchContent`) and pinned to exact versions. Nothing is vendored in
this directory yet; it is reserved for offline mirrors.

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
