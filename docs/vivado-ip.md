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

Without a component file, a VHDL component declaration of the IP in the
sources (`component <instance> port (...)`, in an architecture or a package)
gives the port list, as long as every width is literal (`std_logic`,
`std_logic_vector(8 downto 0)`, `(16-1 downto 0)`); a width that depends on a
generic or a constant is not guessed, and two different declarations of the
same name are not used. The model's note names the declaration it used.

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
| Transceivers Wizard (`xilinx.com:ip:gtwizard_ultrascale`), UltraScale/UltraScale+ GTH and GTY | Ports: those of a JSON `.xci`, otherwise the ports the configuration enables (`INTERNAL_PORT_ENABLED_*` greater than 0, `ENABLE_OPTIONAL_PORTS`) with the widths of the transceiver channel times `C_TOTAL_NUM_CHANNELS` (user data `C_TX_USER_DATA_WIDTH`/`C_RX_USER_DATA_WIDTH` per channel; `drpaddr_in` 9 bits on GTHE3, 10 otherwise; `txdiffctrl_in` 4 bits on GTHE3, 5 otherwise). Clocks: the TX and RX source clocks (`gtwiz_userclk_*_srcclk_out`, or `txoutclk_out`/`rxoutclk_out`) are clocks of their own, period from `C_*_USRCLK_FREQUENCY` x `C_*_USER_CLOCKING_RATIO_FSRC_FUSRCLK`; `usrclk` and `usrclk2` are derived from them (`C_*_USRCLK2_FREQUENCY`), so `get_clocks -of_objects` on the source clock pin finds all three as in Vivado. With user clocking outside the IP (`txusrclk2_in`/`rxusrclk2_in`), the user's clocks. Domains: user data, 8b/10b, gearbox, comma alignment, polarity, buffer status, reset-done and the buffer bypass helper (`gtwiz_buffbypass_*_start_user_in`, `_done_out`, `_error_out`) in the TX or RX `usrclk2`; `drp*` in `drpclk_in`; `gtwiz_reset_rx_cdr_stable_out` and the QPLL reset outputs in `gtwiz_reset_clk_freerun_in`. PMA/PRGDIV reset done, power good, CDR and CPLL lock, `rxsyncdone_out`, `rxphaligndone_out`, `rxdlysresetdone_out`, `cpll*clklost_out` are asynchronous. Not read: resets synchronized in the IP (`gtwiz_reset_*`, `gtwiz_buffbypass_*_reset_in`, channel PMA/PCS/CDR resets), static and analog controls (`loopback_in`, TX swing and emphasis, `txinhibit_in`, CPLL controls), reference clocks and serial pins. A port outside these lists, or user clocks without configured frequencies, makes the IP not modelled (`FE-XCI-UNSUPPORTED`, naming the ports). Verified with the 30 Transceivers Wizard configurations of SURF (PGPv2b, PGPv2fc, PGPv3) and three SURF wrappers. |
| 7-series Transceivers Wizard (`xilinx.com:ip:gtwizard` 3.x), GTP, GTX, GTH | The ports come from the instance (a JSON `.xci`, the component file, or a VHDL component declaration): the configuration alone does not determine them. Ports `gt<N>_<port>` per channel. Clocks: `gt<N>_txoutclk_out`/`rxoutclk_out` are clocks of their own with the period of line rate / internal data width (`gt<N>_val_*_line_rate`, `gt<N>_val_*_int_datawidth`; TXOUTCLKPMA, the AUTO source), `*outclkfabric_out` with the reference clock (`gt<N>_val_*_reference_clock`), `txoutclkpcs_out` without a period; the user clocks are the clocks the design connects to `gt<N>_txusrclk2_in`/`rxusrclk2_in`. Domains: user data, gearbox, 8b/10b, comma alignment, polarity, PRBS and reset-done in the channel's `usrclk2`; `drp*` in `gt<N>_drpclk_in`; the reset state machines' outputs (`*_fsm_reset_done_out`, `*_mmcm_reset_out`, `qpllreset_out`, `pll0reset_out`) in `sysclk_in`. Asynchronous: eye scan, digital and RX monitors, CPLL lock and clock-lost, PMA reset done, phase alignment status, `rxelecidle_out`. Not read: resets and lock inputs synchronized in the IP, analog and static controls, reference and QPLL clocks, serial pins. Other ports, or a domain without its clock port, make the IP not modelled (`FE-XCI-UNSUPPORTED`, naming them). Verified with SURF's PGPv3 GTX (3, 6 and 10 Gb/s) and GTP (3 and 6 Gb/s) wrappers: the RXOUTCLK periods match the comments and MMCM settings in SURF. |
| 10G/25G Ethernet PCS/PMA (`xilinx.com:ip:ten_gig_eth_pcs_pma`), core without shared logic | Ports from the instance (JSON `.xci`, component file or VHDL component declaration). PG068: the XGMII, `configuration_vector`, `status_vector`, `core_status`, `pma_pmd_type`, `tx_disable` and the reset-done outputs are in the TX clock source, `coreclk` on 7 series (and UltraScale 10GBASE-KR), `txusrclk2` on UltraScale 10GBASE-R; the XGMII RX side likewise with the elastic buffer, in the recovered clock without it (`c_no_ebuff`). The DRP arbitration (`drp_*`, `core_to_gt_drp*`, `gt_drp*`) in `dclk`; transceiver debug ports in `txusrclk2` (TX) or the recovered clock (RX). `txoutclk` and `rxrecclk_out` are clocks of their own (322.27 MHz at 10G). `signal_detect`, `tx_fault`, the resets and the QPLL inputs are not read; eye scan, digital monitor, PMA reset done, `qpll0reset` and `reset_tx_bufg_gt` are asynchronous. Verified with SURF's 10G Ethernet on GTX/GTH 7 series and GTH UltraScale. |
| 10G/25G High Speed Ethernet subsystem (`xilinx.com:ip:xxv_ethernet`), Ethernet PCS/PMA 64-bit with the MII data path, one core | Ports from the instance. PG210: `tx_mii_clk` and `rx_clk_out` are clocks of their own (156.25 MHz at 10G, 390.625 MHz at 25G), `rxrecclkout` too; TX MII, `ctl_tx_*`, `stat_tx_*` and the TX transceiver debug ports in `tx_mii_clk`; RX MII in `rx_core_clk` (asynchronous clocking, the RX FIFO crosses into it); `ctl_rx_*`, `stat_rx_*` and the RX debug ports in `rx_clk_out`; the transceiver reset helper's outputs (`gt_reset_*_done_out`, `gtwiz_reset_qpll*reset_out`) in `dclk`; the DRP in `gt_drpclk`. The MAC variants and several cores are not modelled. Verified with SURF's 10G Ethernet on GTH and GTY UltraScale+. |
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
