-- Asynchronous FIFO (Gray-coded pointers), after C. E. Cummings,
-- "Simulation and Synthesis Techniques for Asynchronous FIFO Design", SNUG 2002.
-- VHDL version of examples/async_fifo/rtl/*.v (same structure and names).
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

-- Dual-port memory: written in the write clock domain, read asynchronously.
entity fifomem is
  generic (DSIZE : natural := 8; ASIZE : natural := 4);
  port (
    rdata  : out std_logic_vector(DSIZE-1 downto 0);
    wdata  : in  std_logic_vector(DSIZE-1 downto 0);
    waddr  : in  std_logic_vector(ASIZE-1 downto 0);
    raddr  : in  std_logic_vector(ASIZE-1 downto 0);
    wclken : in  std_logic;
    wfull  : in  std_logic;
    wclk   : in  std_logic);
end entity;

architecture rtl of fifomem is
  constant DEPTH : natural := 2**ASIZE;
  type mem_t is array (0 to DEPTH-1) of std_logic_vector(DSIZE-1 downto 0);
  signal mem : mem_t;
begin
  rdata <= mem(to_integer(unsigned(raddr)));

  process (wclk)
  begin
    if rising_edge(wclk) then
      if wclken = '1' and wfull = '0' then
        mem(to_integer(unsigned(waddr))) <= wdata;
      end if;
    end if;
  end process;
end architecture;

library ieee;
use ieee.std_logic_1164.all;

-- Read pointer (Gray) synchronized into the write clock domain.
entity sync_r2w is
  generic (ASIZE : natural := 4);
  port (
    wq2_rptr : out std_logic_vector(ASIZE downto 0);
    rptr     : in  std_logic_vector(ASIZE downto 0);
    wclk     : in  std_logic;
    wrst_n   : in  std_logic);
end entity;

architecture rtl of sync_r2w is
  signal wq1_rptr : std_logic_vector(ASIZE downto 0);
begin
  process (wclk, wrst_n)
  begin
    if wrst_n = '0' then
      (wq2_rptr, wq1_rptr) <= std_logic_vector'(2*ASIZE+1 downto 0 => '0');
    elsif rising_edge(wclk) then
      (wq2_rptr, wq1_rptr) <= wq1_rptr & rptr;    -- VHDL-2008 aggregate target
    end if;
  end process;
end architecture;

library ieee;
use ieee.std_logic_1164.all;

-- Write pointer (Gray) synchronized into the read clock domain.
entity sync_w2r is
  generic (ASIZE : natural := 4);
  port (
    rq2_wptr : out std_logic_vector(ASIZE downto 0);
    wptr     : in  std_logic_vector(ASIZE downto 0);
    rclk     : in  std_logic;
    rrst_n   : in  std_logic);
end entity;

architecture rtl of sync_w2r is
  signal rq1_wptr : std_logic_vector(ASIZE downto 0);
begin
  process (rclk, rrst_n)
  begin
    if rrst_n = '0' then
      rq1_wptr <= (others => '0');
      rq2_wptr <= (others => '0');
    elsif rising_edge(rclk) then
      rq1_wptr <= wptr;        -- signal assignments: the order does not matter
      rq2_wptr <= rq1_wptr;
    end if;
  end process;
end architecture;

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

-- Read pointer and empty flag (read clock domain).
entity rptr_empty is
  generic (ASIZE : natural := 4);
  port (
    rempty   : out std_logic;
    raddr    : out std_logic_vector(ASIZE-1 downto 0);
    rptr     : out std_logic_vector(ASIZE downto 0);
    rq2_wptr : in  std_logic_vector(ASIZE downto 0);
    rinc     : in  std_logic;
    rclk     : in  std_logic;
    rrst_n   : in  std_logic);
end entity;

architecture rtl of rptr_empty is
  signal rbin                : unsigned(ASIZE downto 0);
  signal rgraynext, rbinnext : unsigned(ASIZE downto 0);
  signal rempty_val          : std_logic;
begin
  process (rclk, rrst_n)
  begin
    if rrst_n = '0' then
      rbin <= (others => '0');
      rptr <= (others => '0');
    elsif rising_edge(rclk) then
      rbin <= rbinnext;
      rptr <= std_logic_vector(rgraynext);
    end if;
  end process;

  raddr      <= std_logic_vector(rbin(ASIZE-1 downto 0));
  rbinnext   <= rbin + (rinc and not rempty);
  rgraynext  <= shift_right(rbinnext, 1) xor rbinnext;
  rempty_val <= '1' when std_logic_vector(rgraynext) = rq2_wptr else '0';

  process (rclk, rrst_n)
  begin
    if rrst_n = '0' then
      rempty <= '1';
    elsif rising_edge(rclk) then
      rempty <= rempty_val;
    end if;
  end process;
end architecture;

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

-- Write pointer and full flag (write clock domain).
entity wptr_full is
  generic (ASIZE : natural := 4);
  port (
    wfull    : out std_logic;
    waddr    : out std_logic_vector(ASIZE-1 downto 0);
    wptr     : out std_logic_vector(ASIZE downto 0);
    wq2_rptr : in  std_logic_vector(ASIZE downto 0);
    winc     : in  std_logic;
    wclk     : in  std_logic;
    wrst_n   : in  std_logic);
end entity;

architecture rtl of wptr_full is
  signal wbin                : unsigned(ASIZE downto 0);
  signal wgraynext, wbinnext : unsigned(ASIZE downto 0);
  signal wfull_val           : std_logic;
begin
  process (wclk, wrst_n)
  begin
    if wrst_n = '0' then
      wbin <= (others => '0');
      wptr <= (others => '0');
    elsif rising_edge(wclk) then
      wbin <= wbinnext;
      wptr <= std_logic_vector(wgraynext);
    end if;
  end process;

  waddr     <= std_logic_vector(wbin(ASIZE-1 downto 0));
  wbinnext  <= wbin + (winc and not wfull);
  wgraynext <= (wbinnext srl 1) xor wbinnext;
  wfull_val <= '1' when std_logic_vector(wgraynext) =
                        (not wq2_rptr(ASIZE downto ASIZE-1)) & wq2_rptr(ASIZE-2 downto 0) else '0';

  process (wclk, wrst_n)
  begin
    if wrst_n = '0' then
      wfull <= '0';
    elsif rising_edge(wclk) then
      wfull <= wfull_val;
    end if;
  end process;
end architecture;

library ieee;
use ieee.std_logic_1164.all;

entity afifo is
  generic (DSIZE : natural := 8; ASIZE : natural := 4);
  port (
    rdata  : out std_logic_vector(DSIZE-1 downto 0);
    wfull  : out std_logic;
    rempty : out std_logic;
    wdata  : in  std_logic_vector(DSIZE-1 downto 0);
    winc, wclk, wrst_n : in std_logic;
    rinc, rclk, rrst_n : in std_logic);
end entity;

architecture rtl of afifo is
  signal waddr, raddr                     : std_logic_vector(ASIZE-1 downto 0);
  signal wptr, rptr, wq2_rptr, rq2_wptr   : std_logic_vector(ASIZE downto 0);
begin
  u_sync_r2w : entity work.sync_r2w generic map (ASIZE)
    port map (wq2_rptr => wq2_rptr, rptr => rptr, wclk => wclk, wrst_n => wrst_n);
  u_sync_w2r : entity work.sync_w2r generic map (ASIZE)
    port map (rq2_wptr => rq2_wptr, wptr => wptr, rclk => rclk, rrst_n => rrst_n);

  u_mem : entity work.fifomem generic map (DSIZE, ASIZE)
    port map (rdata => rdata, wdata => wdata, waddr => waddr, raddr => raddr,
              wclken => winc, wfull => wfull, wclk => wclk);

  u_rptr_empty : entity work.rptr_empty generic map (ASIZE)
    port map (rempty => rempty, raddr => raddr, rptr => rptr, rq2_wptr => rq2_wptr,
              rinc => rinc, rclk => rclk, rrst_n => rrst_n);

  u_wptr_full : entity work.wptr_full generic map (ASIZE)
    port map (wfull => wfull, waddr => waddr, wptr => wptr, wq2_rptr => wq2_rptr,
              winc => winc, wclk => wclk, wrst_n => wrst_n);
end architecture;
