# OpenCDC Analyzer

[![CI](https://github.com/silytoxic-arch/open-CDC/actions/workflows/ci.yml/badge.svg)](https://github.com/silytoxic-arch/open-CDC/actions/workflows/ci.yml)

FPGA clock-domain-crossing (CDC) static analysis tool for Verilog, VHDL and mixed designs. Current status is **stage 20, source-synchronous inputs, Intel SDC idioms, primitive model fixes**.

The architecture is described in [docs/architecture.md](docs/architecture.md).

## What works today (stages 1–20)

| Feature | Status |
|---|---|
| Project file (`*.ocdc.json`) loading and validation | ✅ |
| Verilog-2001/2005 parsing, preprocessing (`include/`define/`ifdef), elaboration (parameters, generate, instance arrays) via slang | ✅ |
| SystemVerilog interfaces: interface instances (with their own logic), modport/generic interface ports, interface arrays, ports passed down the hierarchy; verified on PULP `axi_cdc_intf` | ✅ |
| VHDL-93/2008 (own frontend): libraries and packages (`std`, `ieee` incl. `numeric_std`, `std_logic_arith`, `math_real`), generics, records, arrays and memories, functions/procedures, generate/block, component and entity instantiation, clocked processes in all common styles. Verified on open-logic, neorv32, PoC, OSVVM, surf and the microwatt POWER SoC (ECP5 and Arty A7 builds, the latter with the LiteX-generated LiteDRAM/LiteEth/LiteSDCard cores) | ✅ |
| Mixed Verilog/VHDL designs in both directions, with parameters/generics and ports converted across the border; VHDL records ↔ SystemVerilog packed structs, and VHDL element associations (`p.f`, `p(i)`, `p(h downto l)`) on Verilog ports | ✅ |
| Built-in models of vendor primitives, used when the design instantiates them without a definition: Xilinx UNISIM flip-flops/latches/buffers/SRLs/LUTs/block RAM/DDR/I-O delays/DSP48 slices, BSCANE2 (JTAG clock), XPM CDC macros, `xpm_fifo_async` and XPM memories, MMCM/PLL clock generators (outputs become clocks derived from the input clock, with periods from the multiply/divide parameters), GT transceivers (recovered/transmit clocks as clock sources of their own), Intel `altera_std_synchronizer`/`dcfifo`/`altsyncram`/`altddio`/`altpll`/`altera_pll`, Lattice ECP5/iCE40 PLLs, block RAMs and DDR cells, Gowin PLLs and block RAMs; also from VHDL via `unisim.vcomponents`, `xpm.vcomponents`, `altera_mf.altera_mf_components`, the Lattice (`ecp5u`, iCE40) and Gowin (`gw1n`, `gw2a`, …) `components` packages, or a locally declared component | ✅ |
| Module list and elaborated instance hierarchy | ✅ |
| Frontend diagnostics with file/line, including unsupported or erroneous code | ✅ |
| CLI `opencdc analyze`, with a JSON result | ✅ |
| Qt6 GUI: project explorer, design hierarchy, messages, source viewer with jump-to-source, run report; new/edit/save-as project (sources and their VHDL library, constraints, waivers, parameters, defines, include directories, options) | ✅ |
| RTL extraction into a word-level Design IR: registers with clock, async resets, sync reset (also written as a comparison, `if (rstn == 1'b0)`) and enable; latches; combinational cells; memories; instance tree | ✅ |
| `opencdc analyze --dump-ir`, and a JSON register list | ✅ |
| SDC/XDC subset: `create_clock`, `create_generated_clock`, `set_clock_groups`, `get_ports`/`get_pins`/`get_nets`/`get_clocks` (globs, bit selects, Vivado flip-flop pins by name `<reg>_reg/Q`, `C`, `D`, `CE`, `R`, `S`, `CLR`, `PRE` (e.g. ADI's `get_pins -hier *rst_sync_reg/PRE`), `get_clocks -of_objects [get_nets …]` in clock groups and `set_input_delay -clock`, clock-level `set_false_path` / `set_max_delay -datapath_only` (`-from [get_clocks a] -to [get_clocks b]`), abbreviated options such as `-of`) | ✅ |
| Register-level timing exceptions: `set_false_path` and `set_max_delay -datapath_only` between registers, pins, ports and clocks, with one side alone (`-to` reset pins) or mixed (`-from <clock> -to <registers>`); a crossing they cover whose clock relation is unknown is analyzed as asynchronous, with the constraint named | ✅ |
| Vendor Tcl constraint scripts: `foreach`, `if`/`elseif`/`else`, `for`, `while`, `expr`, `llength`/`lindex`/`lappend`/`incr`, `get_property` (clock `PERIOD`, cell properties); `get_cells` returns flip-flops and instances, with `-hierarchical`, `-regexp` and `-filter` (`NAME`, `PARENT`, `REF_NAME`, `ORIG_REF_NAME`, `IS_SEQUENTIAL`, HDL attributes such as `ASYNC_REG`); netlist traversal with `get_pins`/`get_nets`/`get_cells -of_objects` (flip-flop pins `C D Q CE CLR PRE R S`, `REF_PIN_NAME`, `DIRECTION`, `IS_LEAF`, `IS_PRESET`, …). Verified with the constraint scripts shipped with alexforencich's verilog-ethernet (`axis_async_fifo.tcl`, `sync_reset.tcl`, `eth_mac_fifo.tcl`, `mii_phy_if.tcl`) | ✅ |
| Tcl procedures (`proc` with default and `args` parameters, `return`, `global`) and `source` (relative to the sourcing file, then the project's `tclSourceDirs`); all constraint files of an analysis share one interpreter, as in Vivado and Quartus | ✅ |
| Quartus SDC: `get_registers`/`get_keepers`/`get_nodes` and `get_cells`/`get_pins` with `|`-separated names (registers named after their HDL signal), `get_collection_size`, `foreach_in_collection`, `get_node_info -name`; pin paths into IP internals (`pll|auto_generated|pll1|clk[0]`) resolved to the IP's port; periods with units (`"40.000 ns"`, `{125 MHz}`); `derive_pll_clocks`; register-to-register `set_max_delay` and `-from *`. Verified with the Quartus scripts of verilog-ethernet's DE2-115 example; full names with `entity:instance` levels (`*altera_std_synchronizer:*|din_s1`) | ✅ |
| Source-synchronous inputs: an input timed against a virtual clock (`set_input_delay`) and captured by a clock of the same period is timed by STA, not a crossing (noted as `CDC-SOURCE-SYNC`) | ✅ |
| Scoped constraints (Vivado `read_xdc -ref <module>` / `SCOPED_TO_REF`): a constraint file run once in every instance of a module, with names relative to the instance and `get_ports` naming the instance's ports — as IP libraries ship them (open-logic, hdl-modules). From a script (`read_xdc -ref`, also `-cells`) or in the project (`{"file": "x.tcl", "ref": "module"}`). Abbreviated queries (`get_cell`, `get_pin`, …) are accepted, as in Vivado | ✅ |
| `current_instance` (names relative to an instance; `..`; reset by `current_instance` without an instance and at the end of each file; `get_ports` keeps naming the top-level ports, `get_ports -scoped_to_current_instance` the instance's pins), `get_ports -of_objects <nets>`. Flip-flop cells and pins are one per bit, named as Vivado names them (`x_reg[3]`, also `x_reg[0]` for a one-bit vector), also for `-filter {NAME =~ …}` | ✅ |
| Output ports: an output timed against a clock (`set_output_delay -clock`) must be driven by registers (or inputs) of a clock synchronous to it; a driver of an asynchronous or unrelated clock is reported as `CDC-OUTPUT-CLOCK`. Forwarded clocks (`create_generated_clock` on the output port) and virtual clocks of the driver's period are synchronous; `set_false_path -to` the port exempts it | ✅ |
| Suggested constraints (`--suggest-sdc`, GUI *Export Suggested Constraints*, HTML report section): `create_clock` for clock inputs and GT/black-box clock pins without a constraint, `create_generated_clock` for fabric dividers, `set_clock_groups` for clock families whose relation is unknown — every guess marked TODO or VERIFY | ✅ |
| Constraint consistency checks: a generator configured for another input period (`CON-INPUT-PERIOD`), a constraint on a generator output that disagrees with the generator (`CON-PERIOD-MISMATCH`), overlapping clock groups or related clocks declared asynchronous (`CON-GROUP-*`); a single `-group` makes its clocks asynchronous to all others, as in SDC | ✅ |
| Vivado clock report cross-check (`--vivado-clocks`, project field `vivadoClockReports`, GUI project editor): `report_clocks` / `report_timing_summary` output read back, clocks matched by source pin or name, period mismatches (`VIV-PERIOD-MISMATCH`) and clocks only one side knows reported | ✅ |
| Clock domain analysis: register clock tracing through hierarchy, inverters, gating, clock muxes, dividers; clock relation matrix (same / synchronous / asynchronous / exclusive / **unknown**, with a reason for each pair) | ✅ |
| GUI Clock Domain Viewer (frequency, derived clocks under their master, period derivation such as `clk_in × 0.5`), JSON `clockAnalysis` section | ✅ |
| Sequential dependency graph: register inputs traced back through logic and hierarchy | ✅ |
| CDC path search between asynchronous / unknown-relation clocks; single-bit / multi-bit; control / data; full hierarchical path | ✅ |
| Synchronizer recognition: 1/2/3/N-flop chains (including vector shift registers and chains across modules) with checked conditions; data qualified by a synchronized enable/select, also when the source data is decoded into the enable, selects the qualifier, or is ANDed with it (`q <= req_sync & f(src)`, `q <= req_sync ? f(src) : 0`). Results are reported in three levels: structure recognized, necessary conditions verified, protocol conditions unverified | ✅ |
| Protocol synchronizers: toggle (pulse), Gray code (encoder and ±1 step proven), request/acknowledge handshake, asynchronous FIFO (memory + Gray pointers in both directions, or a write pointer committed through a handshake as in frame FIFOs), reset synchronizers; grouped into synchronizer structures (all crossings of one FIFO instance form one structure, and reconvergence among its pointers, requests and acknowledges is part of its protocol) | ✅ |
| Sources in any common encoding: UTF-8, UTF-16 with BOM, GB18030/GBK (Chinese comments of Windows editors), Windows-1252; converted to UTF-8 for analysis and the source viewer (`FE-ENCODING` note) | ✅ |
| Vivado IP from its configuration file (`.xci` in the sources, XML and JSON formats): FIFO Generator (native and AXI-Stream, common or independent clocks: an asynchronous FIFO with Gray pointers and reset synchronizers), AXI4-Stream Data FIFO, Clocking Wizard (output clocks derived from the input, with Vivado's clock names `clk_out1_clk_wiz_0` usable in constraints), Block Memory Generator (two-clock RAMs cross), one-clock datapath IP (CORDIC, floating point, ...), the 7-series DDR2/DDR3 memory interface (MIG: `ui_clk` derived from the system clock, named `clk_pll_i` as in Vivado; every user interface port in `ui_clk`), XADC Wizard, AXI GPIO/IIC/Timer/UART/Quad SPI, block design Slice/Concat/Constant, ILA and VIO debug cores; ports from the IP's component file next to an XML `.xci`; other IP (your own, a board vendor's) from its IP repository (`ipRepositories`: its `component.xml` and HDL) (see [docs/vivado-ip.md](docs/vivado-ip.md)). Verified with the 13 FIFO configurations of Digilent's vivado-library, two Digilent IP cores, five Pmod IP block designs and five Digilent board demos (Cmod A7, Nexys A7, and the XADC demos of Arty A7, Basys 3, Zybo Z7) | ✅ |
| Modules defined nowhere (vendor IP cores without their HDL): an error with a hint, or black boxes with project option `"unknownModules": "blackbox"` (CLI `--blackbox-unknown`, GUI project settings) | ✅ |
| Every instance without a definition or built-in model is reported (`FE-UNRESOLVED-INSTANCE`), never silently a black box; a user module whose name differs from a vendor primitive only in case does not hide the primitive's model | ✅ |
| Asynchronous reset crossings (grouped per reset source and clock), reconvergence (bit-level, configurable depth), multi-bit incoherency, pulse loss (with clock periods), multi-domain logic | ✅ |
| Rules CDC-001 … CDC-010, all ten (specs in [docs/rules](docs/rules)) | ✅ |
| GUI CDC Path Viewer, Rule Violation Viewer and Output Ports viewer (outputs driven asynchronously to their `set_output_delay` clock, waived or not; example `examples/output_ports`) with filters, details (structure, reconvergence, grouped registers), fix suggestions and jump-to-source | ✅ |
| CDC schematic: the path, the other sources of its destination logic, synchronizer stages, qualifier, reconvergence point and the whole synchronizer structure (FIFO, handshake), laid out left to right (own layered layout), colored by clock domain; zoom/fit, PNG/SVG export, double-click to source | ✅ |
| View linkage: path/violation selection drives the schematic; hierarchy tree with CDC path and issue counts per instance and an instance filter for the path table | ✅ |
| `opencdc analyze --schematic-dir <dir>`: one SVG schematic per path (no Qt needed) | ✅ |
| Waivers: by rule, path fingerprint, source/destination glob; reason required, optional expiry; waived hits stay visible everywhere ([docs/waivers.md](docs/waivers.md)) | ✅ |
| Crossings summarized by clock pair (paths, synchronized, violations, risk, review) in the CLI, the GUI run report, the HTML report and the JSON result (`clockPairs`) | ✅ |
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
opencdc analyze examples/primitives/primitives.ocdc.json      # MMCM (derived clocks with periods), XPM, block RAM
opencdc analyze my.ocdc.json -q --suggest-sdc suggested.sdc    # constraints for missing clocks and unknown relations
opencdc analyze examples/primitives/primitives.ocdc.json --vivado-clocks examples/primitives/vivado_clocks.rpt   # compare periods with Vivado
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
  "constraints": ["constraints/clocks.sdc", {"file": "lib/olo_base_cc_bits.tcl", "ref": "olo_base_cc_bits"}],
  "options": {"unknownClockRelation": "report", "minSyncStages": 2, "reconvergenceDepth": 2},
  "waivers": ["waivers.json"],
  "vivadoClockReports": ["impl/clocks.rpt"],
  "tclSourceDirs": ["syn/quartus"],
  "ipRepositories": ["../ip_repo"]
}
```

`options.unknownClockRelation` sets how clock pairs with an unknown relation are treated. These are pairs from independent sources that no constraint relates.
* `report` (default): analyze them as asynchronous and flag them for review.
* `assume_async`: analyze them as asynchronous without the review flag.
* `assume_sync`: treat them as synchronous.

Different clock names alone never make two clocks asynchronous.

`options.minSyncStages` (default 2) is the minimum flop-chain length accepted by CDC-002, and the minimum reset synchronizer length for CDC-006.

`waivers` lists waiver files (see [docs/waivers.md](docs/waivers.md)).

`vivadoClockReports` (optional) lists outputs of Vivado `report_clocks` (or `report_timing_summary`). Their clocks are matched to the clock model by source pin or name, and differing periods are reported as `VIV-PERIOD-MISMATCH`. The CLI option `--vivado-clocks <file>` adds a report for one run.

`tclSourceDirs` (optional) lists directories searched for relative `source` paths in constraint scripts, after the directory of the sourcing file. Set it to the directory Quartus or Vivado runs the scripts from when the scripts are written relative to it (e.g. `source ../lib/eth/syn/quartus/eth_mac_1g_rgmii.sdc`). All constraint files of a project share one Tcl interpreter, so procedures defined in one file can be called in the files listed after it.

A `constraints` entry `{"file": …, "ref": "<module>"}` is a scoped constraint file, as Vivado's `read_xdc -ref`: it is run once in every instance of the module (or for the top module itself), each run with its own Tcl variables, with object names relative to the instance (`get_cells s1_reg`), `get_ports` naming the instance's ports, and `-hierarchical` searching below the instance only. A module the design does not instantiate is noted (`CON-UNMATCHED`, info). In the GUI project settings the module is the second column of the constraints table. In a scoped file, `current_instance` moves within the instance only.

`ipRepositories` (optional) lists Vivado IP repositories (`ip_repo_paths`). An IP given as `.xci` that OpenCDC does not model is looked up there by its `component.xml`, and its HDL is analyzed (see [docs/vivado-ip.md](docs/vivado-ip.md)).

`options.unknownModules` (default `error`) sets what happens with instances of modules defined nowhere, typically vendor IP cores whose generated HDL is not in the sources: `error` stops before CDC analysis (the message says how to add the IP's HDL), `blackbox` analyzes them as black boxes with a warning each — their outputs have no clock domain, so crossings through them are not checked.

`options.reconvergenceDepth` (default 2, range 0–8) is the number of register levels after synchronizer outputs that CDC-005 searches for reconvergence.

Output ports are checked when `set_output_delay -clock <clk>` names the clock the receiver samples them with: every register or input with a clock domain that drives the port must be synchronous to that clock (`CDC-OUTPUT-CLOCK` otherwise, one warning per constraint and driver clock). `set_false_path -to [get_ports …]` exempts a port (with `-from [get_clocks …]`, only the drivers of those clocks); a waiver for rule `CDC-OUTPUT-CLOCK` covers a reviewed one (see [docs/waivers.md](docs/waivers.md)). The JSON result (`cdc.outputCrossings`) and the HTML report list them.

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

The complete list, with what each model describes and which pins it ignores, is in [docs/primitives.md](docs/primitives.md).

Designs that instantiate FPGA vendor primitives directly need no simulation libraries: when a primitive is instantiated and the sources don't define it, OpenCDC adds its built-in model (reported once as `FE-PRIMITIVES`). A module of the same name in the sources always wins.

| Library | Primitives |
|---|---|
| Xilinx UNISIM | `FDRE`, `FDSE`, `FDCE`, `FDPE` and the older `FD*` forms, `LDCE`/`LDPE`, `BUFG*`, `BUFH*`, `BUFR`, `BUFIO`, `IBUF*`/`OBUF*`/`IOBUF`, `SRL16E`/`SRLC32E`, `LUT1`–`LUT6_2`, `MUXF7/8`, `VCC`/`GND`; `IDDR`/`IDDRE1`, `ODDR`/`ODDRE1`, `IDELAYE2/3`, `ODELAYE2/3`, `IDELAYCTRL` (`RDY` has no clock domain); `RAMB18E1`, `RAMB36E1`, `RAMB18E2`, `RAMB36E2` (each port in its own clock: data written by one port and read by the other is a crossing; a port whose write enable is tied off writes nothing); `ISERDESE2/3`, `OSERDESE2/3` (serial side on `CLK`, word on `CLKDIV`); `URAM288` (one clock); `BUFR` (`BUFR_DIVIDE`) and `BUFGCE_DIV` (`BUFGCE_DIVIDE`) create divided clocks with their period |
| Xilinx clocking | `MMCME2/3/4_BASE/ADV`, `PLLE2/3/4_BASE/ADV`: every `CLKOUT*` becomes a clock derived from `CLKIN1`, so its domains are synchronous to each other and follow the input clock's constraints. Each output's period is the input period × `DIVCLK_DIVIDE` × `CLKOUTn_DIVIDE[_F]` / `CLKFBOUT_MULT[_F]` (the input period comes from the input clock's constraint, or from `CLKIN1_PERIOD`), so CDC-007 can check pulses; the clock is named after the pin (`top.mmcm.CLKOUT0`). `LOCKED` has no clock domain (a reset released by it must be synchronized, CDC-006) |
| Xilinx XPM | `xpm_cdc_single`, `_array_single`, `_gray`, `_handshake`, `_pulse`, `_async_rst`, `_sync_rst`, `xpm_fifo_async` — recognized as the matching synchronizer structures; `xpm_memory_sdpram`, `_tdpram`, `_spram`, `_dpdistram` (`CLOCKING_MODE = "independent_clock"` crosses clocks, `"common_clock"` does not) |
| Xilinx GT | `GTXE2/GTHE2/GTPE2/GTHE3/GTHE4/GTYE3/GTYE4_CHANNEL` and `_COMMON`, `IBUFDS_GTE3/4`: `RXOUTCLK`/`TXOUTCLK` are clock sources of their own (constrain them like in Vivado: `create_clock -period 3.2 [get_pins u_gt/RXOUTCLK]`), `TXDATA` is captured on `TXUSRCLK2`, `RXDATA` comes out on `RXUSRCLK2`, reset-done/lock outputs have no clock domain |
| Intel / Lattice / Gowin PLLs | `altpll` (multiply/divide), `altera_pll` (frequency strings), Lattice `EHXPLLL` (ECP5), `SB_PLL40_CORE/PAD` (iCE40), Gowin `rPLL`/`PLLVR`: outputs are derived clocks with computed periods, lock outputs have no clock domain (Verilog only) |
| Intel | `altera_std_synchronizer`, `altera_std_synchronizer_bundle`, `altera_reset_synchronizer`, `dcfifo`, `altsyncram` / `altera_syncram` (port B on `clock1` when its address or output register says so), `altddio_in`, `altddio_out` |

VHDL code reaches them through `library unisim; use unisim.vcomponents.all;`, `library xpm; use xpm.vcomponents.all;` or `library altera_mf; use altera_mf.altera_mf_components.all;`. `ASYNC_REG` on an instance (Verilog attribute or VHDL `attribute ASYNC_REG of s1 : label is "TRUE"`) marks a synchronizer stage. Parameters the models do not use are ignored, and connections to pins they do not model (cascade, ECC, ...) are listed as `FE-PRIMITIVE-PORT` information. Other unknown modules remain errors (`FE-UNKNOWN-MODULE`).

#### SystemVerilog interfaces

An interface instance becomes part of the module that declares it (its signals are named `ia.data`, its own `always` blocks and ports are analyzed there). A module with an interface port gets one port per interface member it uses (`b.data`), in the direction of the modport, or — for generic `interface` ports — in the direction of its use. Interface arrays (`bus_if b[2]`) and interface ports passed on to submodules work the same way. Project files with `.sv` sources default to SystemVerilog. Functions declared in interfaces (also through modport `import`) and modport expressions (`input .g(gray)`) work. Not modelled: clocking blocks, virtual interfaces.

#### Editing projects in the GUI

**File → New Project…**, **Edit Project…** (Ctrl+E) and **Save Project As…** open the project settings: sources in compilation order with their VHDL library, constraints (with the module a scoped constraint file is run in), waivers, IP repositories, top parameters/generics, defines, include directories and the analysis options. The project is written back as `*.ocdc.json` with paths relative to the file, and analyzed again.

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
