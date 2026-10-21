-- Smoke-test top level: two independent clock inputs.
library ieee;
use ieee.std_logic_1164.all;

library smoke_lib;
use smoke_lib.smoke_pkg.all;

entity top is
  port (
    clk_a      : in  std_logic;
    rst_a_n    : in  std_logic;
    clk_b      : in  std_logic;
    rst_b_n    : in  std_logic;
    data_out   : out data_t;
    status_out : out status_t);
end entity;

architecture rtl of top is
  signal data_a   : data_t;
  signal valid_a  : std_logic;
  signal status_a : status_t;
begin
  u_tx : entity work.tx_domain
    port map (
      clk_a   => clk_a,
      rst_a_n => rst_a_n,
      data    => data_a,
      valid   => valid_a,
      status  => status_a);

  u_rx : entity work.rx_domain
    port map (
      clk_b      => clk_b,
      rst_b_n    => rst_b_n,
      data_in    => data_a,
      valid_in   => valid_a,
      status_in  => status_a,
      data_out   => data_out,
      status_out => status_out);
end architecture;
