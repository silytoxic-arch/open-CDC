# async_fifo example

Classic asynchronous FIFO with Gray-coded pointers (after C. E. Cummings,
"Simulation and Synthesis Techniques for Asynchronous FIFO Design", SNUG 2002),
written in Verilog-2001. Two independent clocks: `wclk` and `rclk`.

| Crossing | Structure | Expected CDC result (stages 4/5) |
|---|---|---|
| `rptr` (rclk) → `wq1_rptr`/`wq2_rptr` (wclk) | 5-bit Gray code, 2-FF synchronizer per bit | Gray synchronizer, protocol condition "changes by one bit" unverified |
| `wptr` (wclk) → `rq1_wptr`/`rq2_wptr` (rclk) | same, opposite direction | same |
| `mem` (wclk) → `rdata` (read side, asynchronous read via `raddr` in rclk) | dual-port memory | asynchronous FIFO structure |

Stage 2 extracts the RTL of this design (11 registers, 170 bits); see
`tests/ir_extract/golden/afifo.ir` for the reviewed IR dump.

Clock constraints for stage 3 are in `constraints/`. They declare the two clocks asynchronous, so the clock relation matrix shows `asynchronous`. If the constraint file is removed from the project, the relation becomes `unknown`.
