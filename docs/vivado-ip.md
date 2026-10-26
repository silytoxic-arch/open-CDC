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
format, the port list (`boundary`) is taken as it is.

## Supported IP

| IP (VLNV) | Model |
|---|---|
| FIFO Generator (`xilinx.com:ip:fifo_generator`, 12.x/13.x), native interface | Independent clocks: memory written in `wr_clk`, read in `rd_clk`, Gray-coded pointers through `C_SYNCHRONIZER_STAGE` synchronizer stages (recognized as an asynchronous FIFO). `rst` is synchronized into each clock (reset synchronizers); `wr_rst`/`rd_rst` and `srst` are synchronous resets. Common clock: one-clock FIFO. Flags and counts (`full`, `empty`, `almost_*`, `prog_*`, `valid`, `wr_ack`, `overflow`, `underflow`, `wr_data_count`, `rd_data_count`, `data_count`, `wr_rst_busy`, `rd_rst_busy`) are in their own clock. Standard and first-word fall-through reads. Different write and read widths. |
| FIFO Generator, AXI-Stream interface | The same, with `s_aclk`/`m_aclk`, active-low `s_aresetn`, and the payload `tdata tstrb tkeep tlast tid tdest tuser` stored together. `axis_*_data_count`, `axis_prog_*`. |
| AXI4-Stream Data FIFO (`xilinx.com:ip:axis_data_fifo`) | The same with `s_axis_aclk`/`m_axis_aclk` (`IS_ACLK_ASYNC`), the signals of `C_AXIS_SIGNAL_SET`, `axis_wr_data_count`/`axis_rd_data_count`, `almost_*`, `prog_*`. Packet mode is modelled as a plain FIFO. |
| ILA (`xilinx.com:ip:ila`) | Inputs only (`clk`, `probe<i>`); `trig_out` and `trig_in_ack` are in the `clk` domain. Constraints that refer to the inside of debug cores (`*ila_core_inst/*`, `dbg_hub`) are noted (`CON-UNMATCHED` info), not warned. |
| VIO (`xilinx.com:ip:vio`) | `probe_out<i>` are registers in the `clk` domain with their initial value: VIO synchronizes the values set over JTAG to `clk`. `probe_in<i>` are inputs only. |

## Not supported

The following IP gets an `FE-XCI-UNSUPPORTED` warning:
- FIFO Generator with an AXI4 memory-mapped interface;
- ILA in AXI monitor mode from an XML file;
- any other IP, for example `clk_wiz`, `blk_mem_gen` and the AXI interconnect.

The IP then stays an unknown module. Add its generated HDL, or set the
project option `unknownModules` to `blackbox`. `.xcix` files (compressed IP
containers) are not read; extract the `.xci` from them.

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
