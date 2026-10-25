-- OpenCDC: component declarations of the modelled Gowin cells
-- (library gw1n; use gw1n.components.all; and the other device families).
-- The models are Verilog (lattice_gowin.v), bound through the mixed-language linker.
-- OPENCDC-LIBRARIES: gw1n gw1nr gw1ns gw1nz gw2a gw2ar gw5a gw5ast
library ieee;
use ieee.std_logic_1164.all;

package components is
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component rPLL port (CLKIN : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component PLLVR port (CLKIN : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component SDPB port (CLKA : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component DPB port (CLKA : in std_ulogic); end component;
end package;
