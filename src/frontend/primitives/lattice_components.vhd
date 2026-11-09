-- OpenCDC: component declarations of the modelled Lattice cells
-- (library ecp5u; use ecp5u.components.all; and the iCE40 libraries).
-- The models are Verilog (lattice_gowin.v), bound through the mixed-language linker.
-- OPENCDC-LIBRARIES: ecp5u ecp5um ecp5um5g machxo2 machxo3l machxo3d sb_ice40_components_syn ice40up
library ieee;
use ieee.std_logic_1164.all;

package components is
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component EHXPLLL port (CLKI : in std_ulogic); end component;
  component DP8KC port (CLKA : in std_ulogic); end component;
  component PDPW8KC port (CLKW : in std_ulogic); end component;
  component SP8KC port (CLK : in std_ulogic); end component;
  component VHI port (Z : out std_ulogic); end component;
  component VLO port (Z : out std_ulogic); end component;
  component USRMCLK port (USRMCLKI : in std_ulogic; USRMCLKTS : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component IDDRX1F port (SCLK : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component ODDRX1F port (SCLK : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component DP16KD port (CLKA : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component SB_PLL40_CORE port (REFERENCECLK : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component SB_PLL40_PAD port (PACKAGEPIN : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component SB_GB port (USER_SIGNAL_TO_GLOBAL_BUFFER : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component SB_DFF port (C : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component SB_DFFE port (C : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component SB_DFFR port (C : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component SB_DFFS port (C : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component SB_DFFER port (C : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component SB_RAM40_4K port (RCLK : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component SB_RAM40_4KNR port (RCLKN : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component SB_RAM40_4KNW port (RCLK : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component SB_RAM40_4KNRNW port (RCLKN : in std_ulogic); end component;
end package;
