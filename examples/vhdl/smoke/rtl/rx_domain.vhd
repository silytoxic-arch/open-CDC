-- Destination clock domain:
--  * valid  is synchronized with a 2-FF synchronizer
--  * data   is captured directly (deliberately unsynchronized multi-bit crossing)
--  * status is synchronized bit by bit through a generate loop
library ieee;
use ieee.std_logic_1164.all;

library smoke_lib;
use smoke_lib.smoke_pkg.all;

entity rx_domain is
  port (
    clk_b      : in  std_logic;
    rst_b_n    : in  std_logic;
    data_in    : in  data_t;
    valid_in   : in  std_logic;
    status_in  : in  status_t;
    data_out   : out data_t;
    status_out : out status_t);
end entity;

architecture rtl of rx_domain is
  component sync_2ff is
    generic (RESET_VAL : std_logic := '0');
    port (clk, rst_n, d : in std_logic; q : out std_logic);
  end component;
  signal valid_sync : std_logic;
begin
  u_valid_sync : sync_2ff
    port map (
      clk   => clk_b,
      rst_n => rst_b_n,
      d     => valid_in,
      q     => valid_sync);

  process (clk_b, rst_b_n)
  begin
    if rst_b_n = '0' then
      data_out <= (others => '0');
    elsif rising_edge(clk_b) then
      if valid_sync = '1' then
        data_out <= data_in;
      end if;
    end if;
  end process;

  g_status : for i in 0 to STATUS_W - 1 generate
    u_sync : sync_2ff
      port map (
        clk   => clk_b,
        rst_n => rst_b_n,
        d     => status_in(i),
        q     => status_out(i));
  end generate;
end architecture;
