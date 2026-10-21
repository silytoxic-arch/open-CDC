-- VHDL version of examples/cdc_rules/rtl/cdc_rules.v: the same crossings,
-- so the CDC results must be identical (see README.md).
library ieee;
use ieee.std_logic_1164.all;

entity sync_2ff is
  port (
    clk : in  std_logic;
    d   : in  std_logic;
    q   : out std_logic);
end entity sync_2ff;

architecture rtl of sync_2ff is
  signal meta, sync : std_logic;
  attribute ASYNC_REG : string;
  attribute ASYNC_REG of meta : signal is "TRUE";
  attribute ASYNC_REG of sync : signal is "TRUE";
begin
  process (clk)
  begin
    if rising_edge(clk) then
      meta <= d;
      sync <= meta;
    end if;
  end process;
  q <= sync;
end architecture rtl;

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity cdc_rules is
  port (
    clk_a  : in  std_logic;
    clk_b  : in  std_logic;
    clk_c  : in  std_logic;
    din    : in  std_logic_vector(7 downto 0);
    bin    : in  std_logic_vector(7 downto 0);
    dout   : out std_logic_vector(7 downto 0);
    status : out std_logic_vector(7 downto 0));
end entity cdc_rules;

architecture rtl of cdc_rules is
  -- source domain (clk_a)
  signal a_flag, a_go, a_lvl, a_tap, a_req, a_x, a_p, a_q, a_valid, a_tog : std_logic;
  signal a_bus, a_data : std_logic_vector(7 downto 0);
  -- destination domain (clk_b)
  signal b_flag_and          : std_logic;
  signal b_cnt               : unsigned(3 downto 0);
  signal b_lvl_s1, b_lvl_use : std_logic;
  signal b_tap_s1, b_tap_s2, b_tap_bad : std_logic;
  signal b_bus_sum           : std_logic_vector(7 downto 0);
  signal b_req               : std_logic;
  signal b_x_s1, b_x_s2, b_x_s3 : std_logic;
  signal b_valid             : std_logic;
  signal b_data              : std_logic_vector(7 downto 0);
  signal b_or_s1, b_or_s2    : std_logic;
  -- clk_c
  signal c_s1, c_s2          : std_logic;
begin
  src : process (clk_a)
  begin
    if rising_edge(clk_a) then
      a_flag  <= din(0);
      a_go    <= din(1);
      a_lvl   <= din(2);
      a_tap   <= din(3);
      a_req   <= din(4);
      a_x     <= din(5);
      a_p     <= din(6);
      a_q     <= din(7);
      a_valid <= not a_valid;
      a_tog   <= a_tog xor din(0);
      a_bus   <= din;
      a_data  <= std_logic_vector(unsigned(din) + 1);
    end if;
  end process;

  -- 1. CDC-001: single bit through logic, no synchronizer
  -- 2. CDC-001: crossing into a clock enable
  -- 3. CDC-002: a single flop whose output is used directly
  -- 4. CDC-002: two-flop chain whose first stage is tapped
  -- 5. CDC-003: multi-bit bus used directly in arithmetic
  -- 7. Safe: three-flop chain
  -- 9. Needs review: combinational logic in front of the first stage
  dst : process (clk_b)
  begin
    if rising_edge(clk_b) then
      b_flag_and <= a_flag and bin(0);
      if a_go = '1' then
        b_cnt <= b_cnt + 1;
      end if;
      b_lvl_s1  <= a_lvl;
      b_lvl_use <= b_lvl_s1 xor bin(1);
      b_tap_s1  <= a_tap;
      b_tap_s2  <= b_tap_s1;
      b_tap_bad <= b_tap_s1 and bin(2);
      b_bus_sum <= std_logic_vector(unsigned(a_bus) + unsigned(bin));
      b_x_s1    <= a_x;
      b_x_s2    <= b_x_s1;
      b_x_s3    <= b_x_s2;
      b_or_s1   <= a_p or a_q;
      b_or_s2   <= b_or_s1;
    end if;
  end process;

  -- 6. Safe: two-flop synchronizer module
  u_req_sync : entity work.sync_2ff port map (clk => clk_b, d => a_req, q => b_req);

  -- 8. Safe (protocol unverified): data qualified by a synchronized valid
  u_valid_sync : entity work.sync_2ff port map (clk => clk_b, d => a_valid, q => b_valid);
  process (clk_b)
  begin
    if rising_edge(clk_b) then
      if b_valid = '1' then
        b_data <= a_data;
      end if;
    end if;
  end process;

  process (clk_b)
  begin
    if rising_edge(clk_b) then
      dout <= b_data xor b_bus_sum xor (std_logic_vector(b_cnt) & b_flag_and & b_lvl_use & b_tap_s2 & b_tap_bad) xor
              (b_req & b_x_s3 & b_or_s2 & "00000");
    end if;
  end process;

  -- 10. CDC-010: synchronized, but the clock relation is not constrained
  process (clk_c)
  begin
    if rising_edge(clk_c) then
      c_s1 <= a_tog;
      c_s2 <= c_s1;
    end if;
  end process;

  status <= "0000000" & c_s2;
end architecture rtl;
