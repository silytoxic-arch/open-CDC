-- 2-flop synchronizer, compiled into the VHDL library demo_lib
library ieee;
use ieee.std_logic_1164.all;

entity demo_lib_sync is
    port (clk : in std_logic; d : in std_logic; q : out std_logic);
end entity;

architecture rtl of demo_lib_sync is
    signal s1, s2 : std_logic := '0';
    attribute ASYNC_REG : string;
    attribute ASYNC_REG of s1, s2 : signal is "TRUE";
begin
    process (clk)
    begin
        if rising_edge(clk) then
            s1 <= d;
            s2 <= s1;
        end if;
    end process;
    q <= s2;
end architecture;
