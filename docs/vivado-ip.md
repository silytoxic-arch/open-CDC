# Vivado IP from its configuration (.xci)

A design that instantiates Vivado IP (for example `fifo_8X4K u_fifo (...)`)
needs a definition of the IP module. You can provide it in two ways.

- **Add the `.xci` file to the sources.** OpenCDC reads the IP configuration
  and generates a model of the module. The model has the same name and ports
  as the IP.
- Add the HDL that Vivado generates for the IP (`<ip>/synth/<ip>.v` or `.vhd`,
  or the simulation netlist). This also works for IP that OpenCDC does not
  model.

The `.xci` models are **CDC models, not functional models**. They capture
which port belongs to which clock and how the IP crosses between its clocks.
They do not reproduce the IP's exact timing, flags or data. Each model is
reported as `FE-XCI-MODEL` (info), with a one-line summary of the
configuration. If the model drives some output port with a constant, a note
lists those ports.

Both formats of the file are read:
- XML (IP-XACT), written by Vivado up to 2020.1;
- JSON, written by Vivado 2020.2 and later.

The models use the `C_*` model parameters of the configuration. In the JSON
format, the port list (`boundary`) is taken as it is. Next to an XML `.xci`,
Vivado also writes the IP's component file `<instance>.xml` (IP-XACT): when it
is there, the port list is taken from it (resolved widths; ports disabled by
the configuration, `isEnabled` false, are left out). A width that is still an
expression is not guessed: the IP is then not modelled from the port list.

## Supported IP

| IP (VLNV) | Model |
|---|---|
| FIFO Generator (`xilinx.com:ip:fifo_generator`, 12.x/13.x), native interface | Independent clocks: memory written in `wr_clk`, read in `rd_clk`, Gray-coded pointers through `C_SYNCHRONIZER_STAGE` synchronizer stages (recognized as an asynchronous FIFO). `rst` is synchronized into each clock (reset synchronizers); `wr_rst`/`rd_rst` and `srst` are synchronous resets. Common clock: one-clock FIFO. Flags and counts (`full`, `empty`, `almost_*`, `prog_*`, `valid`, `wr_ack`, `overflow`, `underflow`, `wr_data_count`, `rd_data_count`, `data_count`, `wr_rst_busy`, `rd_rst_busy`) are in their own clock. Standard and first-word fall-through reads. Different write and read widths. |
| FIFO Generator, AXI-Stream interface | The same, with `s_aclk`/`m_aclk`, active-low `s_aresetn`, and the payload `tdata tstrb tkeep tlast tid tdest tuser` stored together. `axis_*_data_count`, `axis_prog_*`. |
| AXI4-Stream Data FIFO (`xilinx.com:ip:axis_data_fifo`) | The same with `s_axis_aclk`/`m_axis_aclk` (`IS_ACLK_ASYNC`), the signals of `C_AXIS_SIGNAL_SET`, `axis_wr_data_count`/`axis_rd_data_count`, `almost_*`, `prog_*`. Packet mode is modelled as a plain FIFO. |
| Clocking Wizard (`xilinx.com:ip:clk_wiz`) | An `MMCME2_ADV` (or `PLLE2_ADV` for `PRIMITIVE = PLL`) with the multiply/divide values Vivado computed (`C_MMCM_CLKFBOUT_MULT_F`, `C_MMCM_CLKOUTn_DIVIDE`, ...): the clock analysis derives every output clock from the input clock, as Vivado does. Single-ended or differential input (`IBUFDS`), input clock switchover, active-high or active-low reset, `locked`, external feedback. The output clocks also carry Vivado's names (`clk_out1_clk_wiz_0`, the net on the MMCM output) as aliases: `get_clocks`, `set_clock_groups` and `set_input_delay -clock` can use them. Dynamic reconfiguration is not modelled (the configured frequencies are kept). |
| Block Memory Generator (`xilinx.com:ip:blk_mem_gen`), native interface | Ports by memory type (single-port RAM, simple/true dual-port RAM, single/dual-port ROM). A write port writes on its clock, a read port reads through 1 + output-register stages on its clock: data written on `clka` and read on `clkb` is reported as a crossing (the design must hand over the address/data with a protocol). The model keeps 256 words: the contents do not matter for CDC. |
| CORDIC, Floating-point, Divider, Multiplier, Adder/Subtracter, Accumulator, Counter, Shift RAM, DDS, FIR, CIC, Complex Multiplier, DSP48 Macro | A one-clock datapath: every output is a register of the IP's clock (`aclk`/`CLK`) that depends on every input. The ports come from a JSON `.xci` or the component file next to an XML `.xci`; without it, from an XML `.xci` only for the CORDIC. |
| Memory Interface Generator (`xilinx.com:ip:mig_7series`), one DDR2 or DDR3 controller, native or AXI4 interface | `ui_clk` is a clock derived from the system clock (`sys_clk_i`, or `sys_clk_p`/`sys_clk_n` through `IBUFDS`), with Vivado's name `clk_pll_i` as an alias. Its period comes from the memory configuration that Vivado keeps next to the `.xci` (`XML_INPUT_FILE`, usually `mig_a.prj`: memory clock period x PHY ratio, as a ratio to the configured input clock); without that file, from the configured user interface frequency (`FREQ_HZ`), still related to the system clock. Additional clocks `ui_addn_clk_<n>` (`MMCM_CLKOUT<n>_FREQ`) likewise. Every user interface port (`app_*`, or `s_axi_*` and `aresetn`) belongs to `ui_clk`: inputs are read on `ui_clk`, outputs are `ui_clk` registers. `sys_rst` (with its polarity) is synchronized into `ui_clk` by `u_<ip>_mig/u_<mem>_infrastructure/rstdiv0_sync_r`, the register of the IP, so constraints on it match; `ui_clk_sync_rst` and `mmcm_locked` come from it. Memory pins are driven by `ui_clk` registers (`ck_p`/`ck_n` with 0); `device_temp_i` is not read. |
| AXI GPIO, AXI IIC, AXI Timer, AXI UART Lite, AXI UART 16550 (`axi_gpio`, `axi_iic`, `axi_timer`, `axi_uartlite`, `axi_uart16550`) | One clock, `s_axi_aclk`: every output is a register of it that depends on every input. Inputs from pins are sampled by one register: the input synchronizers inside the IP are not modelled, so an input driven from another clock is reported. The UART 16550 with an external clock (`xin`/`rclk`) is not modelled. Ports from a JSON `.xci` or the component file. |
| AXI Quad SPI (`axi_quad_spi`), standard mode | The AXI side on `s_axi_aclk`, the SPI pins (`io*`, `sck_*`, `ss_*`, `spisel`) on `ext_spi_clk`. With `Async_Clk` the two sides exchange data through two asynchronous FIFOs (recognized as such), otherwise directly: both clocks must then be the same, and different clocks are reported. The AXI4 memory interface (`s_axi4_*`, XIP) and the STARTUP primitive ports are not modelled. Ports from a JSON `.xci` or the component file. |
| Slice, Concat, Constant (`xlslice`, `xlconcat`, `xlconstant`) of block designs | Wiring: `Dout = Din[DIN_FROM:DIN_TO]` (a reversed range reverses the bits), `dout = {In<n-1>, ..., In0}` (`NUM_PORTS`, `IN<i>_WIDTH`), `dout = CONST_VAL` (`CONST_WIDTH`, up to 62 bits). |
| XADC Wizard (`xilinx.com:ip:xadc_wiz`), DRP interface | The dynamic reconfiguration port (`do_out`, `drdy_out`) and the status outputs (`channel_out`, `eoc_out`, `eos_out`, `busy_out`, `alarm_out` and the alarms selected) are registers of `dclk_in`, as on the XADC primitive; the analog inputs (`vp_in`/`vn_in`, the `vauxp<n>`/`vauxn<n>` selected) are not read; JTAG status outputs have no clock domain. With the AXI4-Lite interface: one clock, `s_axi_aclk` (ports from a JSON `.xci` or the component file). The AXI4-Stream output is not modelled. |
| ILA (`xilinx.com:ip:ila`) | Inputs only (`clk`, `probe<i>`); `trig_out` and `trig_in_ack` are in the `clk` domain. Constraints that refer to the inside of debug cores (`*ila_core_inst/*`, `dbg_hub`) are noted (`CON-UNMATCHED` info), not warned. |
| VIO (`xilinx.com:ip:vio`) | `probe_out<i>` are registers in the `clk` domain with their initial value: VIO synchronizes the values set over JTAG to `clk`. `probe_in<i>` are inputs only. |

## IP from an IP repository

IP that OpenCDC does not model, for example your own IP or a board vendor's
(Digilent's `pmod_bridge`, `PWM`), can come from its IP repository, as in
Vivado (`ip_repo_paths`). List the repository directories in the project:

```json
"ipRepositories": ["../vivado-library/ip"]
```

The directories are searched (8 levels deep) for the `component.xml` whose
vendor, library, name and version are those of the `.xci`. From it:
- the top module of the synthesis view (`xilinx_anylanguagesynthesis`, else a
  Verilog or VHDL synthesis view), and the HDL files of its file set (Verilog,
  SystemVerilog, VHDL with its library; constraint and other files are left
  out; a `.xci` in the file set is modelled in turn). SystemVerilog files
  switch the analysis to SystemVerilog;
- the model parameters it declares, with the values configured in the `.xci`
  (string, integer, `reg`/`std_logic_vector` bit strings, boolean).

A wrapper module with the instance name instantiates the top module with these
parameters. Its ports are those of the instance (from a JSON `.xci` or the
component file next to an XML `.xci`); ports the configuration disables are
tied to their default value (inputs) or left open (outputs), as in Vivado's
wrapper. The model is reported as `FE-XCI-MODEL` with the repository file.
Without the instance's port list the IP is not modelled (`FE-XCI-UNSUPPORTED`).

## Not supported

The following IP gets an `FE-XCI-UNSUPPORTED` warning:
- FIFO Generator and Block Memory Generator with an AXI4 interface;
- ILA in AXI monitor mode from an XML file;
- the one-clock datapath IP and AXI peripherals above (except CORDIC) from an XML file without its component file;
- the memory interface with another memory type (QDR II+, RLDRAM, LPDDR2), with ECC or with several controllers;
- any other IP that is not in a listed IP repository, for example the UltraScale memory interface (`ddr4`), DMA and the AXI interconnect.

The IP then stays an unknown module. Add its generated HDL, or set the
project option `unknownModules` to `blackbox`. `.xcix` files (compressed IP
containers) are not read; extract the `.xci` from them.

A VHDL component declaration of such an IP still helps. With the IP as a
black box, its outputs drive their signals: a clock output becomes a clock of
its own (for example the `ui_clk` of a memory interface without a model), and
its registers are analyzed. Constraints that refer to the inside of the black box are noted,
not warned.

## Example

```json
{"format": "opencdc-project", "version": 1, "name": "capture", "top": "top",
 "sources": ["rtl/top.v", "ip/fifo_8X4K/fifo_8X4K.xci"],
 "constraints": ["constr/clocks.xdc"]}
```

```
fifo_8X4K.xci: info[FE-XCI-MODEL]: IP 'fifo_8X4K' (xilinx.com:ip:fifo_generator:13.2) modelled from its
configuration: independent clocks (2-stage synchronizers), 8 x 4096, native
```
