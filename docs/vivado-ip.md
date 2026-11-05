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

A core container (`.xcix`, a ZIP archive of the IP directory: the `.xci`, its
component file and output products) is read as well: it is unpacked into a
temporary directory and its `.xci` modelled as above (the model's note names
the container).

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
| Clocking Wizard (`xilinx.com:ip:clk_wiz`) | An `MMCME2_ADV` (or `PLLE2_ADV` for `PRIMITIVE = PLL`) with the multiply/divide values Vivado computed (`C_MMCM_CLKFBOUT_MULT_F`, `C_MMCM_CLKOUTn_DIVIDE`, ...): the clock analysis derives every output clock from the input clock, as Vivado does. Single-ended or differential input (`IBUFDS`), input clock switchover, active-high or active-low reset, `locked`, external feedback. The output clocks also carry Vivado's names (`clk_out1_clk_wiz_0`, the net on the MMCM output) as aliases: `get_clocks`, `set_clock_groups` and `set_input_delay -clock` can use them. Dynamic reconfiguration is not modelled (the configured frequencies are kept). The multiply/divide values are checked against the configured output frequencies (`C_CLKOUTn_OUT_FREQ`): an older `.xci` of a PLL can keep stale `C_PLL_*` values (all 1) next to the MMCM values Vivado computed for the same frequencies; the MMCM values are then used, with a note. |
| Block Memory Generator (`xilinx.com:ip:blk_mem_gen`), native interface | Ports by memory type (single-port RAM, simple/true dual-port RAM, single/dual-port ROM). A write port writes on its clock, a read port reads through 1 + output-register stages on its clock: data written on `clka` and read on `clkb` is reported as a crossing (the design must hand over the address/data with a protocol). The model keeps 256 words: the contents do not matter for CDC. With the AXI4 or AXI4-Lite interface (`C_INTERFACE_TYPE` 1, PG058): every port in `s_aclk`; the ports from the configuration (32-bit addresses, `C_WRITE_WIDTH_A` data, IDs with `C_HAS_AXI_ID`, ECC ports with ECC). |
| CORDIC, Floating-point, Divider, Multiplier, Adder/Subtracter, Accumulator, Counter, Shift RAM, DDS, FIR, CIC, Complex Multiplier, DSP48 Macro | A one-clock datapath: every output is a register of the IP's clock (`aclk`/`CLK`) that depends on every input. The ports come from a JSON `.xci` or the component file next to an XML `.xci`; without it, from an XML `.xci` for the CORDIC and the Floating-point IP (PG060: the A, B, C and operation channels, `tready` unless non-blocking, `tuser`/`tlast` when configured). |
| Memory Interface Generator (`xilinx.com:ip:mig_7series`), one DDR2 or DDR3 controller, native or AXI4 interface | `ui_clk` is a clock derived from the system clock (`sys_clk_i`, or `sys_clk_p`/`sys_clk_n` through `IBUFDS`), with Vivado's name `clk_pll_i` as an alias. Its period comes from the memory configuration that Vivado keeps next to the `.xci` (`XML_INPUT_FILE`, usually `mig_a.prj`: memory clock period x PHY ratio, as a ratio to the configured input clock); without that file, from the configured user interface frequency (`FREQ_HZ`), still related to the system clock. Additional clocks `ui_addn_clk_<n>` (`MMCM_CLKOUT<n>_FREQ`) likewise. Every user interface port (`app_*`, or `s_axi_*` and `aresetn`) belongs to `ui_clk`: inputs are read on `ui_clk`, outputs are `ui_clk` registers. `sys_rst` (with its polarity) is synchronized into `ui_clk` by `u_<ip>_mig/u_<mem>_infrastructure/rstdiv0_sync_r`, the register of the IP, so constraints on it match; `ui_clk_sync_rst` and `mmcm_locked` come from it. Memory pins are driven by `ui_clk` registers (`ck_p`/`ck_n` with 0); `device_temp_i` is not read. |
| AXI GPIO, AXI IIC, AXI Timer, AXI UART Lite, AXI UART 16550 (`axi_gpio`, `axi_iic`, `axi_timer`, `axi_uartlite`, `axi_uart16550`) | One clock, `s_axi_aclk`: every output is a register of it that depends on every input. Inputs from pins are sampled by one register: the input synchronizers inside the IP are not modelled, so an input driven from another clock is reported. The UART 16550 with an external clock (`xin`/`rclk`) is not modelled. Ports from a JSON `.xci` or the component file. |
| AXI Quad SPI (`axi_quad_spi`) | The AXI side on `s_axi_aclk`, the SPI pins (`io*`, `sck_*`, `ss_*`, `spisel`) on `ext_spi_clk`; the AXI4 memory interface (`s_axi4_*`, `C_TYPE_OF_AXI4_INTERFACE` 1, also XIP) on `s_axi4_aclk`. With `Async_Clk` each AXI side exchanges data with the SPI side through two asynchronous FIFOs (recognized as such), otherwise directly: the clocks must then be the same, and different clocks are reported. With the STARTUP primitive (`C_USE_STARTUP`) the model instantiates `STARTUPE2` (`STARTUPE3` on UltraScale) as the IP does: the SPI clock drives its `USRCCLKO` pin (constraints on `*USRCCLKO` match), `cfgclk`/`cfgmclk` are clocks of the configuration oscillator, `eos`/`preq` have no clock domain. Ports from a JSON `.xci` or the component file. |
| Slice, Concat, Constant (`xlslice`, `xlconcat`, `xlconstant`) of block designs | Wiring: `Dout = Din[DIN_FROM:DIN_TO]` (a reversed range reverses the bits), `dout = {In<n-1>, ..., In0}` (`NUM_PORTS`, `IN<i>_WIDTH`), `dout = CONST_VAL` (`CONST_WIDTH`, up to 62 bits). |
| XADC Wizard (`xilinx.com:ip:xadc_wiz`), DRP interface | The dynamic reconfiguration port (`do_out`, `drdy_out`) and the status outputs (`channel_out`, `eoc_out`, `eos_out`, `busy_out`, `alarm_out` and the alarms selected) are registers of `dclk_in`, as on the XADC primitive; the analog inputs (`vp_in`/`vn_in`, the `vauxp<n>`/`vauxn<n>` selected) are not read; JTAG status outputs have no clock domain. With the AXI4-Lite interface: one clock, `s_axi_aclk` (ports from a JSON `.xci` or the component file). The AXI4-Stream output is not modelled. |
| Transceivers Wizard (`xilinx.com:ip:gtwizard_ultrascale`), UltraScale/UltraScale+ GTH and GTY | Ports: those of a JSON `.xci`, otherwise the ports the configuration enables (`INTERNAL_PORT_ENABLED_*` greater than 0, `ENABLE_OPTIONAL_PORTS`) with the widths of the transceiver channel times `C_TOTAL_NUM_CHANNELS` (user data `C_TX_USER_DATA_WIDTH`/`C_RX_USER_DATA_WIDTH` per channel; `drpaddr_in` 9 bits on GTHE3, 10 otherwise; `txdiffctrl_in` 4 bits on GTHE3, 5 otherwise). Clocks: the TX and RX source clocks (`gtwiz_userclk_*_srcclk_out`, or `txoutclk_out`/`rxoutclk_out`) are clocks of their own, period from `C_*_USRCLK_FREQUENCY` x `C_*_USER_CLOCKING_RATIO_FSRC_FUSRCLK`; `usrclk` and `usrclk2` are derived from them (`C_*_USRCLK2_FREQUENCY`), so `get_clocks -of_objects` on the source clock pin finds all three as in Vivado. With user clocking outside the IP (`txusrclk2_in`/`rxusrclk2_in`), the user's clocks. Domains: user data, 8b/10b, gearbox, comma alignment, polarity, buffer status, reset-done and the buffer bypass helper (`gtwiz_buffbypass_*_start_user_in`, `_done_out`, `_error_out`) in the TX or RX `usrclk2`; `drp*` in `drpclk_in`; `gtwiz_reset_rx_cdr_stable_out` and the QPLL reset outputs in `gtwiz_reset_clk_freerun_in`. PMA/PRGDIV reset done, power good, CDR and CPLL lock, `rxsyncdone_out`, `rxphaligndone_out`, `rxdlysresetdone_out`, `cpll*clklost_out` are asynchronous. Not read: resets synchronized in the IP (`gtwiz_reset_*`, `gtwiz_buffbypass_*_reset_in`, channel PMA/PCS/CDR resets), static and analog controls (`loopback_in`, TX swing and emphasis, `txinhibit_in`, CPLL controls), reference clocks and serial pins. A port outside these lists, or user clocks without configured frequencies, makes the IP not modelled (`FE-XCI-UNSUPPORTED`, naming the ports). Verified with the 30 Transceivers Wizard configurations of SURF (PGPv2b, PGPv2fc, PGPv3) and three SURF wrappers. |
| 7-series Transceivers Wizard (`xilinx.com:ip:gtwizard` 3.x), GTP, GTX, GTH | The ports come from the instance (a JSON `.xci`, the component file, or a VHDL component declaration): the configuration alone does not determine them. Ports `gt<N>_<port>` per channel. Clocks: `gt<N>_txoutclk_out`/`rxoutclk_out` are clocks of their own with the period of line rate / internal data width (`gt<N>_val_*_line_rate`, `gt<N>_val_*_int_datawidth`; TXOUTCLKPMA, the AUTO source), `*outclkfabric_out` with the reference clock (`gt<N>_val_*_reference_clock`), `txoutclkpcs_out` without a period; the user clocks are the clocks the design connects to `gt<N>_txusrclk2_in`/`rxusrclk2_in`. Domains: user data, gearbox, 8b/10b, comma alignment, polarity, PRBS and reset-done in the channel's `usrclk2`; `drp*` in `gt<N>_drpclk_in`; the reset state machines' outputs (`*_fsm_reset_done_out`, `*_mmcm_reset_out`, `qpllreset_out`, `pll0reset_out`) in `sysclk_in`. Asynchronous: eye scan, digital and RX monitors, CPLL lock and clock-lost, PMA reset done, phase alignment status, `rxelecidle_out`. Not read: resets and lock inputs synchronized in the IP, analog and static controls, reference and QPLL clocks, serial pins. Other ports, or a domain without its clock port, make the IP not modelled (`FE-XCI-UNSUPPORTED`, naming them). Verified with SURF's PGPv3 GTX (3, 6 and 10 Gb/s) and GTP (3 and 6 Gb/s) wrappers: the RXOUTCLK periods match the comments and MMCM settings in SURF. |
| 10G/25G Ethernet PCS/PMA (`xilinx.com:ip:ten_gig_eth_pcs_pma`), core without shared logic | Ports from the instance (JSON `.xci`, component file or VHDL component declaration). PG068: the XGMII, `configuration_vector`, `status_vector`, `core_status`, `pma_pmd_type`, `tx_disable` and the reset-done outputs are in the TX clock source, `coreclk` on 7 series (and UltraScale 10GBASE-KR), `txusrclk2` on UltraScale 10GBASE-R; the XGMII RX side likewise with the elastic buffer, in the recovered clock without it (`c_no_ebuff`). The DRP arbitration (`drp_*`, `core_to_gt_drp*`, `gt_drp*`) in `dclk`; transceiver debug ports in `txusrclk2` (TX) or the recovered clock (RX). `txoutclk` and `rxrecclk_out` are clocks of their own (322.27 MHz at 10G). `signal_detect`, `tx_fault`, the resets and the QPLL inputs are not read; eye scan, digital monitor, PMA reset done, `qpll0reset` and `reset_tx_bufg_gt` are asynchronous. Verified with SURF's 10G Ethernet on GTX/GTH 7 series and GTH UltraScale. |
| 10G/25G High Speed Ethernet subsystem (`xilinx.com:ip:xxv_ethernet`), Ethernet PCS/PMA 64-bit with the MII data path, one core | Ports from the instance. PG210: `tx_mii_clk` and `rx_clk_out` are clocks of their own (156.25 MHz at 10G, 390.625 MHz at 25G), `rxrecclkout` too; TX MII, `ctl_tx_*`, `stat_tx_*` and the TX transceiver debug ports in `tx_mii_clk`; RX MII in `rx_core_clk` (asynchronous clocking, the RX FIFO crosses into it); `ctl_rx_*`, `stat_rx_*` and the RX debug ports in `rx_clk_out`; the transceiver reset helper's outputs (`gt_reset_*_done_out`, `gtwiz_reset_qpll*reset_out`) in `dclk`; the DRP in `gt_drpclk`. The MAC variants and several cores are not modelled. Verified with SURF's 10G Ethernet on GTH and GTY UltraScale+. |
| 1G/2.5G Ethernet PCS/PMA or SGMII (`xilinx.com:ip:gig_ethernet_pcs_pma`), with a device transceiver and without the shared logic, or synchronous SGMII over UltraScale LVDS with the shared logic, one lane | Ports from the instance. PG047: GMII, `configuration_vector`, `status_vector`, auto-negotiation, `speed_is_*` and `gmii_isolate` in the GMII clock — `userclk2` with a transceiver (GMII RX in `rxuserclk2` when `RXOUTCLK` is its clock source), `clk125_out` over LVDS; transceiver: `txoutclk`/`rxoutclk` are clocks of their own (line rate / 20: 62.5 MHz at 1G, 156.25 MHz at 2.5G), the TX debug ports in `userclk`, the RX debug ports in `rxuserclk2`, the DRP in `gt0_drpclk_in`; `resetdone`, `cplllock`, `mmcm_reset`, eye scan and monitor outputs are asynchronous; reset, `pma_reset`, `mmcm_locked` and `signal_detect` are synchronized in the IP. LVDS: `clk625_out`, `clk312_out` and `clk125_out` are derived from `refclk625_p` by the IP's MMCM (related to it, periods 1.6 × 1, 2, 5 ns without a constraint on the reference); `sgmii_clk_*` and `rst_125_out` in `clk125_out`. Not modelled: the ten-bit interface, the transceiver variant with the shared logic, asynchronous SGMII over LVDS, 7-series LVDS, several lanes, MDIO. Verified with SURF's 1G Ethernet (GTP7 and UltraScale LVDS). |
| XAUI (`xilinx.com:ip:xaui`) with the shared logic in the core (the output clock `clk156_out`) | Ports from the instance. PG053: `clk156_out` is a clock of its own (lane 0's TXOUTCLK, lane rate / 20: 156.25 MHz, 312.5 MHz for DXAUI) and the user clock of all four lanes; XGMII, `configuration_vector`, `status_vector`, `debug` and the transceiver status/control (`gt_txresetdone`, `gt_rxbufstatus`, `gt_rxdisperr`, `gt_rxpolarity`, PRBS, ...) in `clk156_out`; the DRP (`gt<N>_drp*`) in `dclk`; `clk156_lock`, PMA reset done, phase alignment, QPLL lock, eye scan, monitor and power-good outputs are asynchronous; reset, `signal_detect`, the transceiver resets and static controls are synchronized in the IP or static. Not modelled: the core without the shared logic, MDIO. Verified with SURF's XAUI on GTH UltraScale and GTH/GTY UltraScale+. |
| 100G Ethernet (`xilinx.com:ip:cmac_usplus`), CAUI-4 with the AXI4-Stream user interface | Ports from the instance. PG203: `gt_txusrclk2` and `gt_rxusrclk2` are clocks of their own (line rate / 80: 322.27 MHz), `gt_rxrecclkout` too (no period); `gt_ref_clk_out` is derived from `gt_ref_clk_p` (the reference clock through the IP, `C_GT_REF_CLK_FREQ`); TX AXI4-Stream, `tx_preamblein`, `tx_ovfout`/`tx_unfout`, `ctl_tx_*`, `stat_tx_*`, `usr_tx_reset` and the TX transceiver debug ports in `gt_txusrclk2`; RX AXI4-Stream, `rx_preambleout`, `rx_otn_*`, `ctl_rx_*`, `stat_rx_*` (with RS-FEC) and `usr_rx_reset` in `rx_clk` (the design's choice: `gt_txusrclk2` in synchronous mode); the RX transceiver debug ports in `gt_rxusrclk2`; the core DRP in `drp_clk`, the transceiver and common DRPs in `gt_drpclk`; eye scan and power-good outputs are asynchronous; `sys_reset`, `core_*_reset` and `gtwiz_reset_*` are synchronized in the IP. Not modelled: LBUS, CAUI-10, auto-negotiation/link training, the AXI4-Lite interface. Verified with SURF's CAUI-4 wrapper (161.13 and 156.25 MHz reference). |
| IEEE 802.3 Clause 74 FEC (`xilinx.com:ip:ieee802d3_clause74_fec`) | Ports from the instance. PG303: the encoder (`tx_reset`, `tx_din`, `tx_din_start`, `tx_dout`, `tx_dout_start`) in `tx_clk`, the decoder (`rx_reset`, `rx_din`, `rx_din_slip`, `rx_dout`, `rx_dout_start`, `ctrl_rx_*`, `stat_rx_*`) in `rx_clk`; the resets are synchronous to their clock; nothing crosses inside the IP. Verified with SURF's PGPv3/PGPv4 GTY wrapper at 25 Gb/s with the FEC. |
| AXI DMA (`xilinx.com:ip:axi_dma`) in direct register mode | Ports from the instance. PG021: the AXI4-Lite registers, `axi_resetn` and the interrupts (`mm2s_introut`, `s2mm_introut`) in `s_axi_lite_aclk`; with asynchronous clocks (`C_PRMRY_IS_ACLK_ASYNC` 1) the MM2S channel (`m_axi_mm2s_*`, `m_axis_mm2s_*`, `mm2s_prmry_reset_out_n`) in `m_axi_mm2s_aclk` and the S2MM channel in `m_axi_s2mm_aclk`, the IP crossing between them; with synchronous clocks PG021 requires one clock on all clock ports, so every port is in `s_axi_lite_aclk` and another clock on `m_axi_*_aclk` shows as crossings into the IP; `axi_dma_tstvec` is asynchronous. Not modelled: scatter gather, the multichannel DMA. Verified with Digilent's usb2device. |
| AXI Clock Converter (`xilinx.com:ip:axi_clock_converter`) | PG164: the slave interface and `s_axi_aresetn` in `s_axi_aclk`, the master interface and `m_axi_aresetn` in `m_axi_aclk`; the IP crosses between them (asynchronous FIFOs, or a synchronous integer ratio). Ports from the instance or, for an XML `.xci` instantiated from Verilog, from the configuration: AXI4 (8-bit lengths, region, QoS), AXI3 (4-bit lengths, 2-bit lock, QoS, WID) or AXI4-Lite, `C_AXI_ID_WIDTH`/`ADDR`/`DATA` widths, user signals when enabled. Verified with the Loongson NSCSCC SoC (NonTrivialMIPS). |
| AXI Crossbar (`xilinx.com:ip:axi_crossbar`) | PG059: one clock, `aclk`, for every interface and `aresetn`. Ports from the instance or from the configuration (as the clock converter, `C_NUM_SLAVE_SLOTS`/`C_NUM_MASTER_SLOTS` interfaces concatenated in each port). Verified with the Loongson NSCSCC SoC (NonTrivialMIPS). |
| AXI Interconnect (`xilinx.com:ip:axi_interconnect` 1.x) | PG059: each slave port `Sxx_AXI_*` and the master port `M00_AXI_*` in their own clock (`Sxx_AXI_ACLK`, `M00_AXI_ACLK`), with the reset outputs `Sxx_AXI_ARESET_OUT_N`; `INTERCONNECT_ARESETN` in `INTERCONNECT_ACLK`; the IP crosses between the clocks (the summary lists the ports configured asynchronous, `C_Sxx_AXI_IS_ACLK_ASYNC`). The ports come from the instance (JSON `.xci`, component file or VHDL component declaration). Verified with the Loongson chiplab SoC. |
| Processor System Reset (`xilinx.com:ip:proc_sys_reset`) | PG164: `ext_reset_in`, `aux_reset_in`, `mb_debug_sys_rst` and `dcm_locked` are synchronized into `slowest_sync_clk` inside the IP (not read by the model); `mb_reset`, `bus_struct_reset`, `peripheral_reset`, `interconnect_aresetn`, `peripheral_aresetn` are registers of `slowest_sync_clk`. |
| Utility Vector Logic (`xilinx.com:ip:util_vector_logic`) | Combinational: `Res = Op1 & Op2` (`|`, `^`), or `~Op1` (`C_OPERATION`, `C_SIZE`). |
| AXI Protocol Converter, AXI Data FIFO, AXI Register Slice, AXI4-Stream Register Slice / Data Width Converter / Subset Converter / Broadcaster / Combiner | One clock, `aclk`, for every interface, `aresetn` and `aclken`. |
| AXI Data Width Converter (`xilinx.com:ip:axi_dwidth_converter`) | With asynchronous clocks (`C_AXI_IS_ACLK_ASYNC`) or the synchronous ratio FIFO (`C_FIFO_MODE` 2): the slave side in `s_axi_aclk`, the master side in `m_axi_aclk`, the IP crossing between them; otherwise the converter runs on one clock and every port is in `s_axi_aclk` (another clock on `m_axi_aclk` shows as crossings into the IP). |
| AXI4-Stream Clock Converter (`xilinx.com:ip:axis_clock_converter`) | The slave side in `s_axis_aclk`, the master side in `m_axis_aclk` (`C_IS_ACLK_ASYNC`); the IP crosses between them. |
| AXI BRAM Controller (`xilinx.com:ip:axi_bram_ctrl`) | Every port in `s_axi_aclk` (AXI4, AXI4-Lite control, BRAM ports, ECC outputs); `bram_clk_a`/`bram_clk_b` pass `s_axi_aclk` on, so the Block Memory behind it runs on the same clock. |
| AXI APB Bridge (`xilinx.com:ip:axi_apb_bridge`) | One clock, `s_axi_aclk`, for the AXI and the APB side. |
| AXI Interrupt Controller (`xilinx.com:ip:axi_intc`) | PG099: the registers in `s_axi_aclk`, `irq` in `s_axi_aclk` (in `processor_clk` with `C_ENABLE_ASYNC`); interrupt inputs marked asynchronous in `C_ASYNC_INTR` are synchronized inside the IP (not read by the model), the others are sampled in `s_axi_aclk` (all of them with `C_DISABLE_SYNCHRONIZERS`). Not modelled: fast interrupt mode, cascade mode. |
| AXI Ethernet Lite (`xilinx.com:ip:axi_ethernetlite`), MII | PG135: the registers, `ip2intc_irpt`, `phy_rst_n` and MDIO (`phy_mdc`, `phy_mdio_o/_t`) in `s_axi_aclk`; `phy_rx_data`, `phy_dv`, `phy_rx_er` in `phy_rx_clk`; `phy_tx_data`, `phy_tx_en` in `phy_tx_clk`; `phy_crs`, `phy_col`, `phy_mdio_i` are synchronized inside the IP. |
| System ILA (`xilinx.com:ip:system_ila`) | Inputs only. |
| Zynq-7000 Processing System (`xilinx.com:ip:processing_system7`) | Ports from the instance. PG082/UG585: `FCLK_CLK0..3` are clocks of their own (the PS PLLs; period from `PCW_ACT_FPGAn_PERIPHERAL_FREQMHZ`) carrying Vivado's names `clk_fpga_0..3`, so `get_clocks clk_fpga_0` works in the user's constraints; the AXI ports in their interface clocks (`M_AXI_GPn_ACLK`, `S_AXI_GPn_ACLK`, `S_AXI_HPn_ACLK`, `S_AXI_ACP_ACLK`), the DMA request interfaces in `DMAn_ACLK`, EMIO GMII in `ENETn_GMII_TX_CLK`/`RX_CLK`; the EMIO GPIO outputs are registers of the PS clock `cpu_1x` (the CPU clock / 6 or / 4, a clock of its own), so fabric logic using them without a synchronizer is reported; `FCLK_RESETn_N`, the interrupts to the fabric and the other EMIO signals have no fabric clock domain; `IRQ_F2P` and the EMIO inputs are synchronized in the PS; MIO, DDR and PS pins are not read. |
| SmartConnect (`xilinx.com:ip:smartconnect`) | PG247: each AXI interface in the clock its clock interface associates with it (`ASSOCIATED_BUSIF` of `aclk`, `aclk1`, ... in the component file or the JSON `.xci`), `aresetn` in `aclk`; the IP crosses between the clocks. Without the associations the IP is not modelled. |
| ILA (`xilinx.com:ip:ila`) | Inputs only (`clk`, `probe<i>`); `trig_out` and `trig_in_ack` are in the `clk` domain. Constraints that refer to the inside of debug cores (`*ila_core_inst/*`, `dbg_hub`) are noted (`CON-UNMATCHED` info), not warned. |
| VIO (`xilinx.com:ip:vio`) | `probe_out<i>` are registers in the `clk` domain with their initial value: VIO synchronizes the values set over JTAG to `clk`. `probe_in<i>` are inputs only. |

## Block designs (.bd)

A block design file (`.bd`, JSON since Vivado 2014.1) can be listed in the
sources as Vivado lists it in the project. OpenCDC generates the module
Vivado would generate for it (named after the design, for example `soc`, as
the design's top or the module that the user's top instantiates) and models
its IP cells from their configurations:

- **Cells.** Every IP cell is an instance of the module named after its
  configuration (`xci_name`); the configuration `ip/<xci_name>/<xci_name>.xci`
  next to the `.bd` is then modelled as above (`FE-XCI-MODEL`). A module
  reference (`module_ref`) is an instance of the referenced module, with the
  HDL parameters the configuration passes; the module must be in the sources,
  as in the Vivado project.
- **Hierarchies.** Every hierarchical cell (also the AXI Interconnect, which
  Vivado builds from crossbar, couplers, clock, width and protocol converters
  and data FIFOs) is a module of its own (`<design>_<path>_imp`) instantiated
  under the cell's name, so instance paths are Vivado's
  (`soc_inst/sys_bus/s00_couplers/auto_cc`) and constraints written with full
  paths match. Its ports are the nets that cross it.
- **Nets.** A net connects the ports in its list; it carries the block design
  net's name (`clk_gen_clk_peripheral`).
- **Interface nets** connect bus interfaces signal by signal through the port
  maps of the cells: the component file `ip/<cell>/<cell>.xml` (or the
  boundary of a JSON `.xci`, or, for a module reference, the port maps in the
  `.bd`). A port map can name a part of a port (an AXI crossbar's slots share
  its ports). The signal nets are named `<interface net>_<signal>`
  (`fuxi_inst_ARADDR`). An interface port of the design becomes one port per
  signal, `<interface>_<signal>` in lower case (`DDR3_addr`, `UART_txd`), as
  in Vivado's module.
- **Vivado 2020.2 and later** keep the output products in `<project>.gen`:
  the component files are looked up there too (`<project>.srcs/sources_1/bd/...`
  → `<project>.gen/sources_1/bd/...`), for the block design and for an XML
  `.xci`.
- **Widths and defaults.** A net takes the width of its driver; narrower
  inputs get its low bits, wider inputs are padded with 0. An input that is not
  connected gets the default value of its component file, as in Vivado.

The `FE-BD-MODULE` info gives the number of cells, hierarchical cells and
IP configurations, with notes for nets with several drivers or without a
driver. A cell whose ports are not known (the output products were not
generated, so `ip/<cell>/<cell>.xml` is missing) is an error (`FE-BD-CELL`):
generate the output products in Vivado. Verified with four Zynq-7000 block designs (PS7, SmartConnect, AXI GPIO, XADC, Vivado 2019.2 to 2021.2) and the Fuxi SoC (45 cells,
three AXI interconnects, MIG, Quad SPI with STARTUP, Ethernet Lite, interrupt
controller, processor system reset).

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
- FIFO Generator with an AXI4 (memory-mapped) interface;
- ILA in AXI monitor mode from an XML file;
- the one-clock datapath IP and AXI peripherals above (except CORDIC) from an XML file without its component file;
- the memory interface with another memory type (QDR II+, RLDRAM, LPDDR2), with ECC or with several controllers;
- any other IP that is not in a listed IP repository, for example the UltraScale memory interface (`ddr4`), AXI DMA with scatter gather, and the AXI TFT controller (`axi_tft`).

The IP then stays an unknown module. Add its generated HDL, or set the
project option `unknownModules` to `blackbox`.

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
