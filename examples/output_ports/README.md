# output_ports

Output ports checked against the clock their `set_output_delay` names (rule `CDC-OUTPUT-CLOCK`).

| Output | Driven from | Sampled with | Result |
|---|---|---|---|
| `io_data` | `clk_io` register | `clk_io` | not reported |
| `io_req` | `clk_sys` request, synchronized into `clk_io` (`u_req_sync`) | `clk_io` | not reported |
| `sys_flag` | `clk_sys` register | `clk_io` | warning `CDC-OUTPUT-CLOCK` |
| `dbg_led` | `clk_sys` register | `clk_io` | waived by `W-LED` (`waivers.json`) |

```
opencdc analyze output_ports.ocdc.json
opencdc-gui output_ports.ocdc.json   # View → Output Ports
```
