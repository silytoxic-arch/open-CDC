-- Synchronizer cells of the cdc_advanced example (VHDL version of
-- sync_2ff.v, sync_vec.v, rst_sync.v and pulse_sync.v).
library ieee;
use ieee.std_logic_1164.all;

-- Two-flop level synchronizer with asynchronous reset.
entity sync_2ff is
  port (
    clk   : in  std_logic;
    rst_n : in  std_logic;
    d     : in  std_logic;
    q     : out std_logic);
end entity;

architecture rtl of sync_2ff is
  signal meta, sync : std_logic;
  attribute ASYNC_REG : string;
  attribute ASYNC_REG of meta, sync : signal is "TRUE";
begin
  process (clk, rst_n)
  begin
    if rst_n = '0' then
      meta <= '0';
      sync <= '0';
    elsif rising_edge(clk) then
      meta <= d;
      sync <= meta;
    end if;
  end process;
  q <= sync;
end architecture;

library ieee;
use ieee.std_logic_1164.all;

-- Vector of two-flop synchronizers (one chain per bit).
entity sync_vec is
  generic (W : positive := 4);
  port (
    clk   : in  std_logic;
    rst_n : in  std_logic;
    d     : in  std_logic_vector(W-1 downto 0);
    q     : out std_logic_vector(W-1 downto 0));
end entity;

architecture rtl of sync_vec is
  signal s1, s2 : std_logic_vector(W-1 downto 0);
  attribute ASYNC_REG : string;
  attribute ASYNC_REG of s1, s2 : signal is "TRUE";
begin
  process (clk, rst_n)
  begin
    if not rst_n then              -- VHDL-2008 condition operator
      s1 <= (others => '0');
      s2 <= (others => '0');
    elsif clk'event and clk = '1' then
      s1 <= d;
      s2 <= s1;
    end if;
  end process;
  q <= s2;
end architecture;

library ieee;
use ieee.std_logic_1164.all;

-- Reset synchronizer: asynchronous assertion, synchronous release.
entity rst_sync is
  port (
    clk       : in  std_logic;
    rst_in_n  : in  std_logic;
    rst_out_n : out std_logic);
end entity;

architecture rtl of rst_sync is
  signal r : std_logic_vector(1 downto 0);
  attribute ASYNC_REG : string;
  attribute ASYNC_REG of r : signal is "TRUE";
begin
  process (clk, rst_in_n)
  begin
    if rst_in_n = '0' then
      r <= "00";
    elsif rising_edge(clk) then
      r <= r(0) & '1';
    end if;
  end process;
  rst_out_n <= r(1);
end architecture;

library ieee;
use ieee.std_logic_1164.all;

-- Toggle (pulse) synchronizer: one source pulse -> one destination pulse.
entity pulse_sync is
  port (
    src_clk   : in  std_logic;
    src_rst_n : in  std_logic;
    src_pulse : in  std_logic;
    dst_clk   : in  std_logic;
    dst_rst_n : in  std_logic;
    dst_pulse : out std_logic);
end entity;

architecture rtl of pulse_sync is
  signal toggle, t_sync, t_last : std_logic;
begin
  process (src_clk, src_rst_n)
  begin
    if src_rst_n = '0' then
      toggle <= '0';
    elsif rising_edge(src_clk) then
      toggle <= toggle xor src_pulse;
    end if;
  end process;

  u_sync : entity work.sync_2ff port map (clk => dst_clk, rst_n => dst_rst_n, d => toggle, q => t_sync);

  process (dst_clk, dst_rst_n)
  begin
    if dst_rst_n = '0' then
      t_last    <= '0';
      dst_pulse <= '0';
    elsif rising_edge(dst_clk) then
      t_last    <= t_sync;
      dst_pulse <= t_sync xor t_last;
    end if;
  end process;
end architecture;
