-- OpenCDC: component declarations of the modelled XPM CDC macros
-- (library xpm; use xpm.vcomponents.all;).
library ieee;
use ieee.std_logic_1164.all;

package vcomponents is
  component xpm_cdc_single
    generic (DEST_SYNC_FF : integer := 4; INIT_SYNC_FF : integer := 0; SIM_ASSERT_CHK : integer := 0;
             SRC_INPUT_REG : integer := 1);
    port (src_clk : in std_logic; src_in : in std_logic; dest_clk : in std_logic; dest_out : out std_logic);
  end component;
  component xpm_cdc_array_single
    generic (DEST_SYNC_FF : integer := 4; INIT_SYNC_FF : integer := 0; SIM_ASSERT_CHK : integer := 0;
             SRC_INPUT_REG : integer := 1; WIDTH : integer := 2);
    port (src_clk : in std_logic; src_in : in std_logic_vector(WIDTH-1 downto 0); dest_clk : in std_logic;
          dest_out : out std_logic_vector(WIDTH-1 downto 0));
  end component;
  component xpm_cdc_gray
    generic (DEST_SYNC_FF : integer := 4; INIT_SYNC_FF : integer := 0; REG_OUTPUT : integer := 0;
             SIM_ASSERT_CHK : integer := 0; SIM_LOSSLESS_GRAY_CHK : integer := 0; WIDTH : integer := 2);
    port (src_clk : in std_logic; src_in_bin : in std_logic_vector(WIDTH-1 downto 0); dest_clk : in std_logic;
          dest_out_bin : out std_logic_vector(WIDTH-1 downto 0));
  end component;
  component xpm_cdc_handshake
    generic (DEST_EXT_HSK : integer := 1; DEST_SYNC_FF : integer := 4; INIT_SYNC_FF : integer := 0;
             SIM_ASSERT_CHK : integer := 0; SRC_SYNC_FF : integer := 4; WIDTH : integer := 1);
    port (src_clk : in std_logic; src_in : in std_logic_vector(WIDTH-1 downto 0); src_send : in std_logic;
          src_rcv : out std_logic; dest_clk : in std_logic; dest_out : out std_logic_vector(WIDTH-1 downto 0);
          dest_req : out std_logic; dest_ack : in std_logic := '0');
  end component;
  component xpm_cdc_pulse
    generic (DEST_SYNC_FF : integer := 4; INIT_SYNC_FF : integer := 0; REG_OUTPUT : integer := 0;
             RST_USED : integer := 1; SIM_ASSERT_CHK : integer := 0);
    port (src_clk : in std_logic; src_pulse : in std_logic; src_rst : in std_logic := '0'; dest_clk : in std_logic;
          dest_rst : in std_logic := '0'; dest_pulse : out std_logic);
  end component;
  component xpm_cdc_async_rst
    generic (DEST_SYNC_FF : integer := 4; INIT_SYNC_FF : integer := 0; RST_ACTIVE_HIGH : integer := 0);
    port (src_arst : in std_logic; dest_clk : in std_logic; dest_arst : out std_logic);
  end component;
  component xpm_cdc_sync_rst
    generic (DEST_SYNC_FF : integer := 4; INIT : integer := 1; INIT_SYNC_FF : integer := 0;
             SIM_ASSERT_CHK : integer := 0);
    port (src_rst : in std_logic; dest_clk : in std_logic; dest_rst : out std_logic);
  end component;
  component xpm_fifo_async
    generic (CASCADE_HEIGHT : integer := 0; CDC_SYNC_STAGES : integer := 2; DOUT_RESET_VALUE : string := "0";
             ECC_MODE : string := "no_ecc"; FIFO_MEMORY_TYPE : string := "auto"; FIFO_READ_LATENCY : integer := 1;
             FIFO_WRITE_DEPTH : integer := 2048; FULL_RESET_VALUE : integer := 0; PROG_EMPTY_THRESH : integer := 10;
             PROG_FULL_THRESH : integer := 10; RD_DATA_COUNT_WIDTH : integer := 1; READ_DATA_WIDTH : integer := 32;
             READ_MODE : string := "std"; RELATED_CLOCKS : integer := 0; SIM_ASSERT_CHK : integer := 0;
             USE_ADV_FEATURES : string := "0707"; WAKEUP_TIME : integer := 0; WRITE_DATA_WIDTH : integer := 32;
             WR_DATA_COUNT_WIDTH : integer := 1);
    port (almost_empty : out std_logic; almost_full : out std_logic; data_valid : out std_logic;
          dbiterr : out std_logic; dout : out std_logic_vector(READ_DATA_WIDTH-1 downto 0); empty : out std_logic;
          full : out std_logic; overflow : out std_logic; prog_empty : out std_logic; prog_full : out std_logic;
          rd_data_count : out std_logic_vector(RD_DATA_COUNT_WIDTH-1 downto 0); rd_rst_busy : out std_logic;
          sbiterr : out std_logic; underflow : out std_logic; wr_ack : out std_logic;
          wr_data_count : out std_logic_vector(WR_DATA_COUNT_WIDTH-1 downto 0); wr_rst_busy : out std_logic;
          din : in std_logic_vector(WRITE_DATA_WIDTH-1 downto 0); injectdbiterr : in std_logic := '0';
          injectsbiterr : in std_logic := '0'; rd_clk : in std_logic; rd_en : in std_logic; rst : in std_logic;
          sleep : in std_logic := '0'; wr_clk : in std_logic; wr_en : in std_logic);
  end component;
end package;
