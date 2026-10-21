# cdc_rules example

A single Verilog-2001 module with ten hand-placed clock domain crossings, one for each situation the stage-4 rules distinguish (stage-5 results added below):
- `clk_a` and `clk_b` are declared asynchronous.
- `clk_c` is constrained but not related to the other clocks, so its relation to them is **unknown**.

| # | Source → destination | Structure | Expected result |
|---|---|---|---|
| 1 | `a_flag` → `b_flag_and` | single bit through an AND gate | **CDC-001** (Error) |
| 2 | `a_go` → `b_cnt` | single bit into a clock enable | **CDC-001** (Error) |
| 3 | `a_lvl` → `b_lvl_s1` | one flop, output used in logic | **CDC-002** (Warning) |
| 4 | `a_tap` → `b_tap_s1` | two-flop chain, first stage tapped | **CDC-002** (Warning) |
| 5 | `a_bus` → `b_bus_sum` | 8-bit bus into an adder | **CDC-003** (Error) |
| 6 | `a_req` → `u_req_sync.meta` | two-flop synchronizer module | safe, two-flop, conditions verified |
| 7 | `a_x` → `b_x_s1` | three-flop chain | safe, three-flop |
| 8 | `a_data` → `b_data` | data enabled by synchronized valid | safe, qualified data, protocol unverified |
| 9 | `a_p`/`a_q` → `b_or_s1` | OR gate in front of a two-flop chain | **CDC-004** (Error) on both paths (stage 5) |
| 10 | `a_tog` → `c_s1` | toggle flop through a two-flop chain into `clk_c` | `toggle`; **CDC-010** (needs review: relation unknown) |

`a_valid` → `u_valid_sync.meta` (the synchronizer of case 8) is a further path. `a_valid` toggles, so since stage 5 it is recognized as a `toggle` synchronizer.

Stage 5 adds case 11. The observation register `dout` XORs `b_data` into every bit; `b_data` is captured under the synchronized `a_valid`. The same bits also get `b_req`, `b_x_s3`, `b_or_s2` and `b_tap_s2`, which are outputs of other `clk_a` synchronizers. Those synchronizers therefore meet the `a_valid` synchronizer in `dout`: **CDC-005** on `a_tap`, `a_x`, `a_p`, `a_q`, `a_req` and `a_valid` (potential risk). This is a side effect of the XOR observation register, and it shows how bit-level reconvergence is reported.

Totals: 12 paths. Hits: CDC-001 ×2, CDC-002 ×2, CDC-003 ×1, CDC-004 ×2, CDC-005 ×6, CDC-010 ×1.

```sh
opencdc analyze examples/cdc_rules/cdc_rules.ocdc.json
```

The CLI exits with code 3 because error-severity violations are present.
