# smoke example

Small Verilog-2001 design with two independent clocks (`clk_a`, `clk_b`).

| Crossing (tx → rx)      | Structure                          | Expected CDC result (from stage 4/5) |
|-------------------------|------------------------------------|--------------------------------------|
| `valid`                 | 2-FF synchronizer `u_valid_sync`   | synchronized                         |
| `data[7:0]`             | captured directly, enabled by synchronized `valid` | MUX/enable-type crossing, protocol unverified |
| `status[3:0]`           | per-bit 2-FF synchronizers (generate) | CDC-008 (multi-bit sync incoherency). Needs review: the synchronized bits only leave the design through a port, so they are not read together inside it |
| `rst_b_n` (input)       | asynchronous reset of 11 `clk_b` registers, no reset synchronizer | CDC-006 potential risk (one grouped path) |

Stages 1–2 parse, elaborate and extract the RTL of this design (see `tests/ir_extract/golden/smoke.ir`). Stages 4–5 produce the CDC results above.

Clock constraints for stage 3 are in `constraints/`. They declare the two clocks asynchronous, so the clock relation matrix shows `asynchronous`. If the constraint file is removed from the project, the relation becomes `unknown`.
