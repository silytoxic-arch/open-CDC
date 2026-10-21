# cdc_advanced example

A multi-module Verilog-2001 design for the stage-5 analysis. It has three independent clocks, declared asynchronous: `clk_a` 10 ns, `clk_b` 8 ns, `clk_c` 12 ns. It combines correctly built protocol synchronizers with **one deliberate bug per stage-5 rule**.

| # | Structure | Where | Expected result |
|---|---|---|---|
| 1 | Reset synchronizers for `clk_a` and `clk_b` (`rst_sync`, `{r[1], r[0]} <= {r[0], 1'b1}`) | `u_rst_a`, `u_rst_b` | `reset-sync`, necessary conditions verified, safe |
| 2 | **Bug**: `clk_c` registers use the reset synchronized for `clk_a` | `rst_c_n = rst_a_n` | **CDC-006** Violation (6 registers grouped into one path) |
| 3 | Asynchronous FIFO with Gray pointers and registered read | `u_fifo` | memory read: `async-fifo`; both pointers: `gray`, verified (increment ≤ 1 proven); safe |
| 4 | Toggle pulse synchronizer with edge detection | `u_pulse` | `toggle`, verified, safe |
| 5 | Four-phase request/acknowledge transfer `clk_a → clk_c` | `u_hs` | data: `handshake` (request + acknowledge paths in one structure), protocol unverified, safe |
| 6 | **Bug**: two flags synchronized separately, then ANDed | `b_go <= start_s & mode_s` | **CDC-005** on both paths (potential risk) |
| 7 | **Bug**: one-cycle pulse into a level synchronizer, 10 ns source vs 8 ns destination | `a_kick` | **CDC-007** (10 ns < 1.5 × 8 ns = 12 ns) |
| 8 | **Bug**: 4-bit configuration word through per-bit synchronizers, compared as a value | `u_cfg`, `b_cfg_hit` | **CDC-008** potential risk |
| 9 | **Bug**: signals of `clk_a` and `clk_c` ORed in front of a synchronizer into `clk_b` | `u_mix` | **CDC-004** and **CDC-009** on both paths |

Expected totals: 16 paths and 11 synchronizer structures. Hits: CDC-004 ×2, CDC-005 ×2, CDC-006 ×1, CDC-007 ×1, CDC-008 ×1, CDC-009 ×2, and no CDC-001/002/003/010. The CLI exits with code 3, because error-severity violations are present (CDC-004, CDC-006).

```sh
opencdc analyze examples/cdc_advanced/cdc_advanced.ocdc.json
```

The design inputs have no `set_input_delay`, so they have no clock domain. The external reset `rst_n` is therefore asynchronous to every clock, and only reaches registers through the two reset synchronizers.
