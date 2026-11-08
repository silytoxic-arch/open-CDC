-- OpenCDC: component declarations of the modelled Intel cells
-- (library altera_mf; use altera_mf.altera_mf_components.all;).
library ieee;
use ieee.std_logic_1164.all;

package altera_mf_components is
  component dcfifo
    generic (add_usedw_msb_bit : string := "OFF"; clocks_are_synchronized : string := "FALSE";
             delay_rdusedw : natural := 1; delay_wrusedw : natural := 1; intended_device_family : string := "";
             enable_ecc : string := "FALSE"; lpm_numwords : natural := 16; lpm_showahead : string := "OFF";
             lpm_type : string := "dcfifo"; lpm_width : natural := 8; lpm_widthu : natural := 4;
             overflow_checking : string := "ON"; rdsync_delaypipe : natural := 4; read_aclr_synch : string := "OFF";
             underflow_checking : string := "ON"; use_eab : string := "ON"; write_aclr_synch : string := "OFF";
             wrsync_delaypipe : natural := 4);
    port (aclr : in std_logic := '0'; data : in std_logic_vector(lpm_width-1 downto 0); rdclk : in std_logic;
          rdreq : in std_logic; wrclk : in std_logic; wrreq : in std_logic;
          q : out std_logic_vector(lpm_width-1 downto 0); rdempty : out std_logic; rdfull : out std_logic;
          rdusedw : out std_logic_vector(lpm_widthu-1 downto 0); wrempty : out std_logic; wrfull : out std_logic;
          wrusedw : out std_logic_vector(lpm_widthu-1 downto 0); eccstatus : out std_logic);
  end component;
  component altera_std_synchronizer
    generic (depth : integer := 3);
    port (clk : in std_logic; reset_n : in std_logic; din : in std_logic; dout : out std_logic);
  end component;
  component altera_std_synchronizer_bundle
    generic (depth : integer := 3; width : integer := 1);
    port (clk : in std_logic; reset_n : in std_logic; din : in std_logic_vector(width-1 downto 0);
          dout : out std_logic_vector(width-1 downto 0));
  end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component altsyncram port (clock0 : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component altddio_in port (inclock : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component altddio_out port (outclock : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component altera_syncram port (clock0 : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component altpll port (inclk : in std_logic_vector(1 downto 0)); end component;
  component scfifo
    generic (add_ram_output_register : string := "OFF"; allow_rwcycle_when_full : string := "OFF";
             almost_empty_value : natural := 0; almost_full_value : natural := 0; enable_ecc : string := "FALSE";
             intended_device_family : string := ""; lpm_hint : string := "UNUSED"; lpm_numwords : natural := 16;
             lpm_showahead : string := "OFF"; lpm_type : string := "scfifo"; lpm_width : natural := 8;
             lpm_widthu : natural := 4; maximum_depth : natural := 0; overflow_checking : string := "ON";
             underflow_checking : string := "ON"; use_eab : string := "ON");
    port (aclr : in std_logic := '0'; almost_empty : out std_logic; almost_full : out std_logic;
          clock : in std_logic; data : in std_logic_vector(lpm_width-1 downto 0); eccstatus : out std_logic;
          empty : out std_logic; full : out std_logic; q : out std_logic_vector(lpm_width-1 downto 0);
          rdreq : in std_logic; sclr : in std_logic := '0'; usedw : out std_logic_vector(lpm_widthu-1 downto 0);
          wrreq : in std_logic);
  end component;
end package;
