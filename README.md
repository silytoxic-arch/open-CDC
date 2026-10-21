# OpenCDC Analyzer

[![CI](https://github.com/silytoxic-arch/open-CDC/actions/workflows/ci.yml/badge.svg)](https://github.com/silytoxic-arch/open-CDC/actions/workflows/ci.yml)

FPGA clock-domain-crossing (CDC) static analysis tool for Verilog, VHDL and mixed designs. Current status is **stage 10, SystemVerilog interfaces, memory/DDR primitives and clock generator frequencies**.

The architecture is described in [docs/architecture.md](docs/architecture.md).

## What works today (stages 1–10)

| Feature | Status |
|---|---|
| Project file (`*.ocdc.json`) loading and validation | ✅ |
| Verilog-2001/2005 parsing, preprocessing (`include/`define/`ifdef), elaboration (parameters, generate, instance arrays) via slang | ✅ |
| SystemVerilog interfaces: interface instances (with their own logic), modport/generic interface ports, interface arrays, ports passed down the hierarchy; verified on PULP `axi_cdc_intf` | ✅ |
| VHDL-93/2008 (own frontend): libraries and packages (`std`, `ieee` incl. `numeric_std`, `std_logic_arith`, `math_real`), generics, records, arrays and memories, functions/procedures, generate/block, component and entity instantiation, clocked processes in all common styles. Verified on open-logic, neorv32, PoC, OSVVM and surf | ✅ |
| Mixed Verilog/VHDL designs in both directions, with parameters/generics and ports converted across the border; VHDL records ↔ SystemVerilog packed structs, and VHDL element associations (`p.f`, `p(i)`, `p(h downto l)`) on Verilog ports | ✅ |
| Built-in models of vendor primitives, used when the design instantiates them without a definition: Xilinx UNISIM flip-flops/latches/buffers/SRLs/LUTs/block RAM/DDR/I-O delays, XPM CDC macros, `xpm_fifo_async` and XPM memories, MMCM/PLL clock generators (outputs become clocks derived from the input clock, with periods from the multiply/divide parameters), Intel `altera_std_synchronizer`/`dcfifo`/`altsyncram`/`altddio`; also from VHDL via `unisim.vcomponents`, `xpm.vcomponents`, `altera_mf.altera_mf_components` | ✅ |
| Module list and elaborated instance hierarchy | ✅ |
| Frontend diagnostics with file/line, including unsupported or erroneous code | ✅ |
| CLI `opencdc analyze`, with a JSON result | ✅ |
| Qt6 GUI: project explorer, design hierarchy, messages, source viewer with jump-to-source, run report; new/edit/save-as project (sources and their VHDL library, constraints, waivers, parameters, defines, include directories, options) | ✅ |
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
| Install rules and packages: Windows ZIP + NSIS installer with the Qt and MSVC runtimes bundled, Linux TGZ/DEB; built and tested by CI on Windows (MSVC `/W4 /WX`) and Linux (GCC, Clang, sanitizers) | ✅ |

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

On Windows this produces `OpenCDC-<version>-windows-x64.zip`, and an NSIS installer when `makensis` is on `PATH`. The installer adds a Start-menu entry and can optionally add the CLI to `PATH`. The Qt runtime (deployed by windeployqt) and the Visual C++ runtime are next to the executables, so neither package needs a Qt installation or the VC++ Redistributable. Every CI run uploads these packages as build artifacts (Actions → the run → Artifacts). Programs are in `bin/`; `docs/`, `examples/` and `tools/` are next to it. On Linux, CPack produces a TGZ and a DEB that use the system Qt; data files go to `share/opencdc/`. The CTest test `package.install_smoke` installs the tree and runs the installed CLI and GUI.

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

Paths are relative to the project file. All sources form one compilation unit, in the order listed, as in FPGA vendor tools. `language` is one of `verilog-2001`, `verilog-2005` or `systemverilog-2017`; without it, a project that has `.sv`/`.svh` sources uses `systemverilog-2017`, otherwise `verilog-2001`. In Verilog modes, SystemVerilog keywords such as `logic` and `bit` are accepted as identifiers.

#### VHDL and mixed designs

Files ending in `.vhd` or `.vhdl` are VHDL (VHDL-2008, which also accepts VHDL-93 code). A source entry can name the VHDL library the file is compiled into; the default is `work`:

```json
"sources": [
  {"file": "olo/olo_base_pkg_math.vhd", "library": "olo"},
  {"file": "olo/olo_base_cc_bits.vhd",  "library": "olo"},
  "rtl/top.v"
],
"parameters": {"Width_g": 8, "MODE": "\"FAST\""}
```

`parameters` overrides generics (VHDL) or parameters (Verilog) of the top level; values are written in the syntax of the top's language. VHDL and Verilog files can be mixed freely: Verilog can instantiate VHDL entities and VHDL can instantiate Verilog modules (as a component or with `entity work.name`), nested to any depth. Names are matched case-insensitively across the border, and port width differences follow the Verilog rule. A VHDL record port connects to a SystemVerilog packed struct (first field = most significant bits, as in both languages), and VHDL can associate fields, elements and slices of a Verilog port separately (`i.hdr.len => x`, `d(0 to 3) => y`). Black boxes are reported as `IR-UNRESOLVED-INSTANCE`.

#### Vendor primitives

Designs that instantiate FPGA vendor primitives directly need no simulation libraries: when a primitive is instantiated and the sources don't define it, OpenCDC adds its built-in model (reported once as `FE-PRIMITIVES`). A module of the same name in the sources always wins.

| Library | Primitives |
|---|---|
| Xilinx UNISIM | `FDRE`, `FDSE`, `FDCE`, `FDPE` and the older `FD*` forms, `LDCE`/`LDPE`, `BUFG*`, `BUFH*`, `BUFR`, `BUFIO`, `IBUF*`/`OBUF*`/`IOBUF`, `SRL16E`/`SRLC32E`, `LUT1`–`LUT6_2`, `MUXF7/8`, `VCC`/`GND`; `IDDR`/`IDDRE1`, `ODDR`/`ODDRE1`, `IDELAYE2/3`, `ODELAYE2/3`, `IDELAYCTRL` (`RDY` has no clock domain); `RAMB18E1`, `RAMB36E1`, `RAMB18E2`, `RAMB36E2` (each port in its own clock: data written by one port and read by the other is a crossing) |
| Xilinx clocking | `MMCME2/3/4_BASE/ADV`, `PLLE2/3/4_BASE/ADV`: every `CLKOUT*` becomes a clock derived from `CLKIN1`, so its domains are synchronous to each other and follow the input clock's constraints. Each output's period is the input period × `DIVCLK_DIVIDE` × `CLKOUTn_DIVIDE[_F]` / `CLKFBOUT_MULT[_F]` (the input period comes from the input clock's constraint, or from `CLKIN1_PERIOD`), so CDC-007 can check pulses; the clock is named after the pin (`top.mmcm.CLKOUT0`). `LOCKED` has no clock domain (a reset released by it must be synchronized, CDC-006) |
| Xilinx XPM | `xpm_cdc_single`, `_array_single`, `_gray`, `_handshake`, `_pulse`, `_async_rst`, `_sync_rst`, `xpm_fifo_async` — recognized as the matching synchronizer structures; `xpm_memory_sdpram`, `_tdpram`, `_spram`, `_dpdistram` (`CLOCKING_MODE = "independent_clock"` crosses clocks, `"common_clock"` does not) |
| Intel | `altera_std_synchronizer`, `altera_std_synchronizer_bundle`, `altera_reset_synchronizer`, `dcfifo`, `altsyncram` (port B on `clock1` when its address or output register says so), `altddio_in`, `altddio_out` |

VHDL code reaches them through `library unisim; use unisim.vcomponents.all;`, `library xpm; use xpm.vcomponents.all;` or `library altera_mf; use altera_mf.altera_mf_components.all;`. `ASYNC_REG` on an instance (Verilog attribute or VHDL `attribute ASYNC_REG of s1 : label is "TRUE"`) marks a synchronizer stage. Parameters the models do not use are ignored, and connections to pins they do not model (cascade, ECC, ...) are listed as `FE-PRIMITIVE-PORT` information. Other unknown modules remain errors (`FE-UNKNOWN-MODULE`).

#### SystemVerilog interfaces

An interface instance becomes part of the module that declares it (its signals are named `ia.data`, its own `always` blocks and ports are analyzed there). A module with an interface port gets one port per interface member it uses (`b.data`), in the direction of the modport, or — for generic `interface` ports — in the direction of its use. Interface arrays (`bus_if b[2]`) and interface ports passed on to submodules work the same way. Project files with `.sv` sources default to SystemVerilog. Not modelled: functions/tasks declared in interfaces, modport expressions, clocking blocks, virtual interfaces.

#### Editing projects in the GUI

**File → New Project…**, **Edit Project…** (Ctrl+E) and **Save Project As…** open the project settings: sources in compilation order with their VHDL library, constraints, waivers, top parameters/generics, defines, include directories and the analysis options. The project is written back as `*.ocdc.json` with paths relative to the file, and analyzed again.

Examples: `examples/vhdl/` has VHDL versions of all four examples, `examples/mixed/` has mixed versions; both give the same CDC results as the Verilog originals.

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
src/frontend/ IFrontend interface, language selection and mixed-language linking;
              slang adapter (the only place slang is used) with the Verilog IR builder;
              vhdl/: own VHDL parser, elaborator and IR builder
src/engine/   project files, waiver files, AnalysisSession facade, JSON / HTML / CSV reports
src/cli/      opencdc command line tool
src/gui/      opencdc-gui (Qt6 Widgets)
tests/        Catch2 unit tests, CLI end-to-end tests, headless GUI tests, install test, benchmark census
tools/bench/  benchmark design generator (gen_bench.py)
tools/ci/     verify-windows.ps1: one-step Windows build/test/package check;
              vhdl-corpus.sh: VHDL regression on pinned open-source projects (run by CI)
              sv-corpus.sh: SystemVerilog regression (PULP AXI CDC through interfaces, run by CI)
tools/vhdl/   opencdc-vhdl-parse / opencdc-vhdl-elab development tools
examples/     real Verilog test designs; vhdl/ and mixed/ versions of them
```
