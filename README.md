# OpenCDC Analyzer

FPGA clock-domain-crossing (CDC) static analysis tool. Current status is **stage 7, engineering (reports, waivers, performance, packaging)**: all planned stages are done.

The architecture is described in [docs/architecture.md](docs/architecture.md).

## What works today (stages 1–7)

| Feature | Status |
|---|---|
| Project file (`*.ocdc.json`) loading and validation | ✅ |
| Verilog-2001/2005 parsing, preprocessing (`include/`define/`ifdef), elaboration (parameters, generate, instance arrays) via slang | ✅ |
| Module list and elaborated instance hierarchy | ✅ |
| Frontend diagnostics with file/line, including unsupported or erroneous code | ✅ |
| CLI `opencdc analyze`, with a JSON result | ✅ |
| Qt6 GUI: project explorer, design hierarchy, messages, source viewer with jump-to-source, run report | ✅ |
| RTL extraction into a word-level Design IR: registers with clock, async resets, sync reset and enable; latches; combinational cells; memories; instance tree | ✅ |
| `opencdc analyze --dump-ir`, and a JSON register list | ✅ |
| SDC/XDC subset: `create_clock`, `create_generated_clock`, `set_clock_groups`, `get_ports`/`get_pins`/`get_nets`/`get_clocks` (globs, bit selects, Vivado `<reg>_reg/Q`/`C` pins) | ✅ |
| Clock domain analysis: register clock tracing through hierarchy, inverters, gating, clock muxes, dividers; clock relation matrix (same / synchronous / asynchronous / exclusive / **unknown**, with a reason for each pair) | ✅ |
| GUI Clock Domain Viewer, JSON `clockAnalysis` section | ✅ |
| Sequential dependency graph: register inputs traced back through logic and hierarchy | ✅ |
| CDC path search between asynchronous / unknown-relation clocks; single-bit / multi-bit; control / data; full hierarchical path | ✅ |
| Synchronizer recognition: 1/2/3/N-flop chains (including vector shift registers and chains across modules) with checked conditions; data qualified by a synchronized enable/select. Results are reported in three levels: structure recognized, necessary conditions verified, protocol conditions unverified | ✅ |
| Protocol synchronizers: toggle (pulse), Gray code (encoder and ±1 step proven), request/acknowledge handshake, asynchronous FIFO (memory + Gray pointers in both directions), reset synchronizers; grouped into synchronizer structures | ✅ |
| Asynchronous reset crossings (grouped per reset source and clock), reconvergence (bit-level, configurable depth), multi-bit incoherency, pulse loss (with clock periods), multi-domain logic | ✅ |
| Rules CDC-001 … CDC-010, all ten (specs in [docs/rules](docs/rules)) | ✅ |
| GUI CDC Path Viewer and Rule Violation Viewer with filters, details (structure, reconvergence, grouped registers), fix suggestions and jump-to-source | ✅ |
| CDC schematic: the path, the other sources of its destination logic, synchronizer stages, qualifier, reconvergence point and the whole synchronizer structure (FIFO, handshake), laid out left to right (own layered layout), colored by clock domain; zoom/fit, PNG/SVG export, double-click to source | ✅ |
| View linkage: path/violation selection drives the schematic; hierarchy tree with CDC path and issue counts per instance and an instance filter for the path table | ✅ |
| `opencdc analyze --schematic-dir <dir>`: one SVG schematic per path (no Qt needed) | ✅ |
| Waivers: by rule, path fingerprint, source/destination glob; reason required, optional expiry; waived hits stay visible everywhere ([docs/waivers.md](docs/waivers.md)) | ✅ |
| Reports: self-contained HTML (summary, stages, clocks, rules, every path with its schematic, structures, waivers, diagnostics, unanalyzed constructs), CSV (paths, rule hits), JSON (streamed) — from the CLI and the GUI | ✅ |
| Scale: 10⁶ registers analyzed in ~19 s / 2.6 GB on 2 vCPUs; GUI tables are model/view ([docs/benchmarks.md](docs/benchmarks.md)) | ✅ |
| Install rules and packages: Windows ZIP + NSIS installer with the Qt runtime deployed (windeployqt), Linux TGZ/DEB | ✅ |

Constructs that can't be modelled are listed as `IR-UNSUPPORTED-*` diagnostics and in the report's "not analyzed" table. They are never silently dropped.

When CDC analysis can't run (HDL errors, no clocks), the engine, the CLI, the JSON output (`"cdcAnalysisAvailable": false`) and the GUI say so explicitly, so partial output can't be mistaken for CDC results.

## Building

### Requirements

* CMake ≥ 3.25
* A C++20 compiler: Visual Studio 2022 (MSVC 19.3x+) on Windows. GCC ≥ 12 or Clang ≥ 17 work for development and CI.
* Python 3, needed by slang's build to generate sources.
* Qt ≥ 6.5 with the Widgets and Concurrent modules. Only the GUI needs Qt; configure with `-DOPENCDC_BUILD_GUI=OFF` to build without it.
* Internet access on first configure. Dependencies come from GitHub via FetchContent; see [third_party/README.md](third_party/README.md) for offline builds.

### Windows x64 / Visual Studio 2022

From a "x64 Native Tools Command Prompt for VS 2022" (or PowerShell):

```bat
set CMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64
cmake --preset windows-msvc
cmake --build --preset windows-msvc-release
ctest --preset windows-msvc-release
```

Use the `windows-msvc-debug` build/test presets for a Debug build. The binaries end up in `build/windows-msvc/bin/<Config>/`. To run the GUI outside of CTest, either put `C:\Qt\6.8.3\msvc2022_64\bin` on `PATH` or run `windeployqt opencdc-gui.exe`.

To verify everything in one step (Debug + Release with warnings as errors, all tests, packaging), run:

```bat
powershell -ExecutionPolicy Bypass -File tools\ci\verify-windows.ps1 -QtDir C:\Qt\6.8.3\msvc2022_64
```

The log is written to `build\verify-windows.log`, and the packages to `dist\`.

You can also open the folder in Visual Studio 2022 ("Open Folder"), which picks up `CMakePresets.json` automatically.

### Installing and packaging

```sh
cmake --install build/<preset> --config Release --prefix <dir>       # installed tree
cpack --config build/<preset>/CPackConfig.cmake -C Release -B dist    # packages
```

On Windows this produces `OpenCDC-<version>-windows-x64.zip`, and an NSIS installer when `makensis` is on `PATH`. The installer adds a Start-menu entry and can optionally add the CLI to `PATH`. `opencdc-gui.exe` comes with the Qt runtime deployed next to it, so neither package needs a Qt installation. Programs are in `bin/`; `docs/`, `examples/` and `tools/` are next to it. On Linux, CPack produces a TGZ and a DEB that use the system Qt; data files go to `share/opencdc/`. The CTest test `package.install_smoke` installs the tree and runs the installed CLI and GUI.

### Linux / macOS (development)

```sh
cmake --preset linux-gcc            # or linux-clang
cmake --build --preset linux-gcc-release
ctest --preset linux-gcc-release
```

`linux-gcc-sanitize` builds the CLI and tests with AddressSanitizer, UndefinedBehaviorSanitizer and the checked libstdc++ (`_GLIBCXX_DEBUG`), and runs the same tests. Use `-j1` on machines with less than 8 GB of memory.

## Usage

```sh
opencdc --version
opencdc analyze examples/smoke/smoke.ocdc.json
opencdc analyze examples/smoke/smoke.ocdc.json --quiet --json result.json
opencdc analyze examples/async_fifo/afifo.ocdc.json --dump-ir afifo.ir
opencdc analyze examples/cdc_rules/cdc_rules.ocdc.json      # one example of every stage-4 rule
opencdc analyze examples/cdc_advanced/cdc_advanced.ocdc.json  # protocol synchronizers + one bug per stage-5 rule
opencdc analyze examples/cdc_advanced/cdc_advanced.ocdc.json -q --schematic-dir schematics   # SVG per path
opencdc analyze examples/cdc_advanced/cdc_advanced_waived.ocdc.json    # same design with two waivers
opencdc analyze examples/cdc_advanced/cdc_advanced.ocdc.json -q --html report.html --csv report/
python3 tools/bench/gen_bench.py soc --domains 8 --blocks 1250 --out bench/soc_100k   # benchmark design
opencdc-gui examples/smoke/smoke.ocdc.json
```

The CLI exit codes are:
* 0: no errors and no error-severity CDC violations.
* 1: the analysis reported errors (HDL or constraint).
* 2: usage or project-file problems.
* 3: error-severity CDC rule violations were found that are not waived.

In the HTML report, a design with more than 5000 paths lists only the paths that need attention: those with rule hits, or a status other than safe. The CSV and JSON outputs always contain every path.

### Project file

```json
{
  "format": "opencdc-project",
  "version": 1,
  "name": "smoke",
  "top": "top",
  "language": "verilog-2001",
  "sources": ["rtl/sync_2ff.v", "rtl/top.v"],
  "includeDirs": ["rtl/include"],
  "defines": {"WIDTH": "8", "USE_FIFO": null},
  "constraints": ["constraints/clocks.sdc"],
  "options": {"unknownClockRelation": "report", "minSyncStages": 2, "reconvergenceDepth": 2},
  "waivers": ["waivers.json"]
}
```

`options.unknownClockRelation` sets how clock pairs with an unknown relation are treated. These are pairs from independent sources that no constraint relates.
* `report` (default): analyze them as asynchronous and flag them for review.
* `assume_async`: analyze them as asynchronous without the review flag.
* `assume_sync`: treat them as synchronous.

Different clock names alone never make two clocks asynchronous.

`options.minSyncStages` (default 2) is the minimum flop-chain length accepted by CDC-002, and the minimum reset synchronizer length for CDC-006.

`waivers` lists waiver files (see [docs/waivers.md](docs/waivers.md)).

`options.reconvergenceDepth` (default 2, range 0–8) is the number of register levels after synchronizer outputs that CDC-005 searches for reconvergence.

Input ports take part in CDC analysis only when `set_input_delay -clock <clk>` assigns them a clock domain. Otherwise they are listed as `CDC-NO-DOMAIN`. Asynchronous resets are the exception: an input without a clock domain that drives asynchronous reset pins is asynchronous to every clock, and its release is checked by CDC-006. Declare a reset that is synchronized outside the design with `set_input_delay -clock`.

Paths are relative to the project file. All sources form one compilation unit, in the order listed, as in FPGA vendor tools. `language` is one of `verilog-2001` (default), `verilog-2005` or `systemverilog-2017`. In Verilog modes, SystemVerilog keywords such as `logic` and `bit` are accepted as identifiers.

## Repository layout

```
cmake/        build helpers (warnings, dependencies, "no Qt in core" check, install/CPack)
docs/         architecture and design documents
src/core/     diagnostics, IDs, Result<T>, progress/cancellation (no Qt, no slang)
src/ir/       Design IR: SigSpec, cells, registers, module bodies, instance tree, dump
src/graph/    DesignView: cross-hierarchy driver lookup over the IR
src/constraints/ SDC/XDC subset interpreter and design object resolver
src/clock/    clock tracing, clock model, relation matrix
src/cdc/      sequential dependency graph, crossing detection, synchronizer recognition,
              advanced analysis (reset/protocol synchronizers, reconvergence)
src/rules/    rule interface, registry, CDC-001 … CDC-010 (specs: docs/rules/), waiver matching
src/schematic/ CDC schematic model, layered layout and SVG output (no Qt)
src/frontend/ IFrontend interface + slang adapter (the only place slang is used),
              including the IR builder (expression lowering, symbolic execution of always blocks)
src/engine/   project files, waiver files, AnalysisSession facade, JSON / HTML / CSV reports
src/cli/      opencdc command line tool
src/gui/      opencdc-gui (Qt6 Widgets)
tests/        Catch2 unit tests, CLI end-to-end tests, headless GUI tests, install test, benchmark census
tools/bench/  benchmark design generator (gen_bench.py)
tools/ci/     verify-windows.ps1: one-step Windows build/test/package check
examples/     real Verilog test designs
```
