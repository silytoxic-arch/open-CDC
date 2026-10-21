# Async FIFO clocks (Vivado XDC style).
set wr_period 8.000
create_clock -period $wr_period -name wclk [get_ports wclk]
create_clock -period 5.000 -name rclk [get_ports rclk]

set_clock_groups -name async_wr_rd -asynchronous \
    -group [get_clocks wclk] \
    -group [get_clocks rclk]

# Synchronizer flops (pins use Vivado's <reg>_reg[i] naming).
set_property ASYNC_REG TRUE [get_cells -hierarchical *q1_*ptr_reg*]
set_max_delay -datapath_only -from [get_pins u_wptr_full/wptr_reg[*]/C] 5.000
