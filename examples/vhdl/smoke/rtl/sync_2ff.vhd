-- Two flip-flop single-bit synchronizer.
library ieee;
use ieee.std_logic_1164.all;

entity sync_2ff is
  generic (RESET_VAL : std_logic := '0');
  port (
    clk   : in  std_logic;
    rst_n : in  std_logic;
    d     : in  std_logic;
    q     : out std_logic);
end entity;

architecture rtl of sync_2ff is
  signal meta : std_logic;
  signal sync : std_logic;
  attribute ASYNC_REG : string;
  attribute ASYNC_REG of meta : signal is "TRUE";
  attribute ASYNC_REG of sync : signal is "TRUE";
begin
  process (clk, rst_n)
  begin
    if not rst_n then
      meta <= RESET_VAL;
      sync <= RESET_VAL;
    elsif rising_edge(clk) then
      meta <= d;
      sync <= meta;
    end if;
  end process;
  q <= sync;
end architecture;
