-- Four-phase request/acknowledge transfer of a data word.
library ieee;
use ieee.std_logic_1164.all;

entity handshake is
  generic (DW : positive := 8);
  port (
    src_clk   : in  std_logic;
    src_rst_n : in  std_logic;
    send      : in  std_logic;
    din       : in  std_logic_vector(DW-1 downto 0);
    busy      : out std_logic;
    dst_clk   : in  std_logic;
    dst_rst_n : in  std_logic;
    dout      : out std_logic_vector(DW-1 downto 0);
    dvalid    : out std_logic);
end entity;

architecture rtl of handshake is
  signal req, ack, req_s, ack_s : std_logic;
  signal data : std_logic_vector(DW-1 downto 0);
begin
  process (src_clk, src_rst_n)
  begin
    if src_rst_n = '0' then
      req  <= '0';
      data <= (others => '0');
    elsif rising_edge(src_clk) then
      if req = '0' and ack_s = '0' and send = '1' then
        req  <= '1';
        data <= din;
      elsif req = '1' and ack_s = '1' then
        req <= '0';
      end if;
    end if;
  end process;
  busy <= req or ack_s;

  u_req : entity work.sync_2ff port map (clk => dst_clk, rst_n => dst_rst_n, d => req, q => req_s);

  process (dst_clk, dst_rst_n)
  begin
    if dst_rst_n = '0' then
      ack    <= '0';
      dout   <= (others => '0');
      dvalid <= '0';
    elsif rising_edge(dst_clk) then
      dvalid <= '0';
      if req_s = '1' and ack = '0' then
        dout   <= data;
        ack    <= '1';
        dvalid <= '1';
      elsif req_s = '0' then
        ack <= '0';
      end if;
    end if;
  end process;

  u_ack : entity work.sync_2ff port map (clk => src_clk, rst_n => src_rst_n, d => ack, q => ack_s);
end architecture;
