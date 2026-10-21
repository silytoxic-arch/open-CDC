-- Source clock domain: produces a data word, a one-cycle valid pulse and a
-- status bus.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

library smoke_lib;
use smoke_lib.smoke_pkg.all;

entity tx_domain is
  port (
    clk_a   : in  std_logic;
    rst_a_n : in  std_logic;
    data    : out data_t;
    valid   : out std_logic;
    status  : out status_t);
end entity;

architecture rtl of tx_domain is
  signal cnt : unsigned(3 downto 0);
begin
  process (clk_a, rst_a_n)
  begin
    if rst_a_n = '0' then
      cnt    <= (others => '0');
      data   <= (others => '0');
      valid  <= '0';
      status <= (others => '0');
    elsif rising_edge(clk_a) then
      cnt   <= cnt + 1;
      valid <= '1' when cnt = x"F" else '0';
      if cnt = x"F" then
        data <= std_logic_vector(unsigned(data) + 1);   -- VHDL-2008: out ports are readable
      end if;
      case cnt(1 downto 0) is
        when "00"   => status <= "0001";
        when "01"   => status <= "0010";
        when "10"   => status <= "0100";
        when others => status <= "1000";
      end case;
    end if;
  end process;
end architecture;
