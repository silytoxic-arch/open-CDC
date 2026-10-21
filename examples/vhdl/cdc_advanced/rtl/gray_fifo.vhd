-- Asynchronous FIFO: Gray-coded pointers synchronized in both directions,
-- registered read data qualified by the synchronized write pointer.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity gray_fifo is
  generic (
    DW : positive := 8;
    AW : positive := 3);
  port (
    wclk   : in  std_logic;
    wrst_n : in  std_logic;
    winc   : in  std_logic;
    wdata  : in  std_logic_vector(DW-1 downto 0);
    wfull  : out std_logic;
    rclk   : in  std_logic;
    rrst_n : in  std_logic;
    rinc   : in  std_logic;
    rdata  : out std_logic_vector(DW-1 downto 0);
    rempty : out std_logic);
end entity;

architecture rtl of gray_fifo is
  type mem_t is array (0 to 2**AW - 1) of std_logic_vector(DW-1 downto 0);
  signal mem : mem_t;

  function bin2gray(b : unsigned) return unsigned is
  begin
    return (b srl 1) xor b;
  end function;

  -- write side
  signal wbin, wptr   : unsigned(AW downto 0);
  signal rptr_w       : std_logic_vector(AW downto 0);   -- read pointer, synchronized to wclk
  signal wbin_next    : unsigned(AW downto 0);
  signal wfull_i      : std_logic;
  -- read side
  signal rbin, rptr   : unsigned(AW downto 0);
  signal wptr_r       : std_logic_vector(AW downto 0);   -- write pointer, synchronized to rclk
  signal rbin_next    : unsigned(AW downto 0);
  signal rempty_i     : std_logic;
begin
  wfull_i   <= '1' when std_logic_vector(wptr) = (not rptr_w(AW downto AW-1)) & rptr_w(AW-2 downto 0) else '0';
  wfull     <= wfull_i;
  wbin_next <= wbin + 1 when (winc and not wfull_i) = '1' else wbin;

  process (wclk, wrst_n)
  begin
    if wrst_n = '0' then
      wbin <= (others => '0');
      wptr <= (others => '0');
    elsif rising_edge(wclk) then
      wbin <= wbin_next;
      wptr <= bin2gray(wbin_next);
    end if;
  end process;

  process (wclk)
  begin
    if rising_edge(wclk) then
      if winc = '1' and wfull_i = '0' then
        mem(to_integer(wbin(AW-1 downto 0))) <= wdata;
      end if;
    end if;
  end process;

  rempty_i  <= '1' when std_logic_vector(rptr) = wptr_r else '0';
  rempty    <= rempty_i;
  rbin_next <= rbin + (rinc and not rempty_i);       -- VHDL-2008: unsigned + std_ulogic

  process (rclk, rrst_n)
  begin
    if rrst_n = '0' then
      rbin  <= (others => '0');
      rptr  <= (others => '0');
      rdata <= (others => '0');
    elsif rising_edge(rclk) then
      rbin <= rbin_next;
      rptr <= bin2gray(rbin_next);
      if rinc = '1' and rempty_i = '0' then
        rdata <= mem(to_integer(rbin(AW-1 downto 0)));
      end if;
    end if;
  end process;

  u_w2r : entity work.sync_vec generic map (W => AW + 1)
    port map (clk => rclk, rst_n => rrst_n, d => std_logic_vector(wptr), q => wptr_r);
  u_r2w : entity work.sync_vec generic map (W => AW + 1)
    port map (clk => wclk, rst_n => wrst_n, d => std_logic_vector(rptr), q => rptr_w);
end architecture;
