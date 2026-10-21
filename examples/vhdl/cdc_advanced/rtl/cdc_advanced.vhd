-- VHDL version of examples/cdc_advanced/rtl/cdc_advanced.v: correct
-- protocol synchronizers next to one deliberate bug per stage-5 rule.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity cdc_advanced is
  port (
    clk_a       : in  std_logic;   -- 100 MHz
    clk_b       : in  std_logic;   -- 125 MHz
    clk_c       : in  std_logic;   --  83 MHz
    rst_n       : in  std_logic;   -- external asynchronous reset
    -- clk_a side
    a_wr        : in  std_logic;
    a_wdata     : in  std_logic_vector(7 downto 0);
    a_event     : in  std_logic;
    a_send      : in  std_logic;
    a_word      : in  std_logic_vector(7 downto 0);
    a_start_set : in  std_logic;
    a_mode_set  : in  std_logic;
    a_cfg_we    : in  std_logic;
    a_cfg_in    : in  std_logic_vector(3 downto 0);
    a_x_in      : in  std_logic;
    a_full      : out std_logic;
    a_busy      : out std_logic;
    -- clk_b side
    b_rd        : in  std_logic;
    b_rdata     : out std_logic_vector(7 downto 0);
    b_empty     : out std_logic;
    b_events    : out std_logic_vector(7 downto 0);
    b_go        : out std_logic;
    b_kicks     : out std_logic_vector(3 downto 0);
    b_cfg_hit   : out std_logic;
    b_mix       : out std_logic;
    -- clk_c side
    c_y_in      : in  std_logic;
    c_word      : out std_logic_vector(7 downto 0);
    c_valid     : out std_logic);
end entity;

architecture rtl of cdc_advanced is
  signal rst_a_n, rst_b_n, rst_c_n : std_logic;
  signal b_event                   : std_logic;
  signal a_start, a_mode           : std_logic;
  signal start_s, mode_s           : std_logic;
  signal a_cnt                     : unsigned(3 downto 0);
  signal a_kick, kick_s            : std_logic;
  signal a_cfg, cfg_s              : std_logic_vector(3 downto 0);
  signal a_x, c_y                  : std_logic;
  signal events                    : unsigned(7 downto 0);
  signal kicks                     : unsigned(3 downto 0);
begin
  -- ---------------- resets ----------------
  -- (1) one reset synchronizer per clock for clk_a and clk_b: safe
  u_rst_a : entity work.rst_sync port map (clk => clk_a, rst_in_n => rst_n, rst_out_n => rst_a_n);
  u_rst_b : entity work.rst_sync port map (clk => clk_b, rst_in_n => rst_n, rst_out_n => rst_b_n);
  -- (2) BUG CDC-006: clk_c logic uses the reset synchronized for clk_a
  rst_c_n <= rst_a_n;

  -- ---------------- (3) asynchronous FIFO clk_a -> clk_b: safe ----------------
  u_fifo : entity work.gray_fifo
    generic map (DW => 8, AW => 3)
    port map (
      wclk => clk_a, wrst_n => rst_a_n, winc => a_wr, wdata => a_wdata, wfull => a_full,
      rclk => clk_b, rrst_n => rst_b_n, rinc => b_rd, rdata => b_rdata, rempty => b_empty);

  -- ---------------- (4) toggle pulse synchronizer clk_a -> clk_b: safe ----------------
  u_pulse : entity work.pulse_sync
    port map (src_clk => clk_a, src_rst_n => rst_a_n, src_pulse => a_event,
              dst_clk => clk_b, dst_rst_n => rst_b_n, dst_pulse => b_event);
  process (clk_b, rst_b_n)
  begin
    if rst_b_n = '0' then
      events <= (others => '0');
    elsif rising_edge(clk_b) then
      if b_event = '1' then
        events <= events + 1;
      end if;
    end if;
  end process;
  b_events <= std_logic_vector(events);

  -- ---------------- (5) handshake clk_a -> clk_c: safe protocol ----------------
  u_hs : entity work.handshake
    generic map (DW => 8)
    port map (src_clk => clk_a, src_rst_n => rst_a_n, send => a_send, din => a_word, busy => a_busy,
              dst_clk => clk_c, dst_rst_n => rst_c_n, dout => c_word, dvalid => c_valid);

  -- ---------------- (6) BUG CDC-005: two synchronized flags reconverge ----------------
  process (clk_a, rst_a_n)
  begin
    if rst_a_n = '0' then
      a_start <= '0';
      a_mode  <= '0';
    elsif rising_edge(clk_a) then
      if a_start_set = '1' then
        a_start <= a_word(0);
      end if;
      if a_mode_set = '1' then
        a_mode <= a_word(1);
      end if;
    end if;
  end process;
  u_start : entity work.sync_2ff port map (clk => clk_b, rst_n => rst_b_n, d => a_start, q => start_s);
  u_mode  : entity work.sync_2ff port map (clk => clk_b, rst_n => rst_b_n, d => a_mode,  q => mode_s);
  process (clk_b, rst_b_n)
  begin
    if rst_b_n = '0' then
      b_go <= '0';
    elsif rising_edge(clk_b) then
      b_go <= start_s and mode_s;
    end if;
  end process;

  -- ---------------- (7) BUG CDC-007: one-cycle pulse into a level synchronizer ----------------
  process (clk_a, rst_a_n)
  begin
    if rst_a_n = '0' then
      a_cnt  <= (others => '0');
      a_kick <= '0';
    elsif rising_edge(clk_a) then
      a_cnt <= a_cnt + 1;
      if a_cnt = 7 then             -- high for one clk_a cycle out of 16
        a_kick <= '1';
      else
        a_kick <= '0';
      end if;
    end if;
  end process;
  u_kick : entity work.sync_2ff port map (clk => clk_b, rst_n => rst_b_n, d => a_kick, q => kick_s);
  process (clk_b, rst_b_n)
  begin
    if rst_b_n = '0' then
      kicks <= (others => '0');
    elsif rising_edge(clk_b) then
      if kick_s = '1' then
        kicks <= kicks + 1;
      end if;
    end if;
  end process;
  b_kicks <= std_logic_vector(kicks);

  -- ---------------- (8) BUG CDC-008: configuration word synchronized bit by bit ----------------
  process (clk_a, rst_a_n)
  begin
    if rst_a_n = '0' then
      a_cfg <= (others => '0');
    elsif rising_edge(clk_a) then
      if a_cfg_we = '1' then
        a_cfg <= a_cfg_in;
      end if;
    end if;
  end process;
  u_cfg : entity work.sync_vec generic map (W => 4) port map (clk => clk_b, rst_n => rst_b_n, d => a_cfg, q => cfg_s);
  process (clk_b, rst_b_n)
  begin
    if rst_b_n = '0' then
      b_cfg_hit <= '0';
    elsif rising_edge(clk_b) then
      b_cfg_hit <= '1' when cfg_s = x"A" else '0';   -- VHDL-2008 sequential conditional assignment
    end if;
  end process;

  -- ---------------- (9) BUG CDC-004 + CDC-009: two foreign clocks ORed before a synchronizer ----------------
  process (clk_a, rst_a_n)
  begin
    if rst_a_n = '0' then
      a_x <= '0';
    elsif rising_edge(clk_a) then
      a_x <= a_x_in;
    end if;
  end process;
  process (clk_c, rst_c_n)
  begin
    if rst_c_n = '0' then
      c_y <= '0';
    elsif rising_edge(clk_c) then
      c_y <= c_y_in;
    end if;
  end process;
  u_mix : entity work.sync_2ff port map (clk => clk_b, rst_n => rst_b_n, d => a_x or c_y, q => b_mix);
end architecture;
