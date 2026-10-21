-- Shared definitions for the smoke example (compiled into library smoke_lib).
library ieee;
use ieee.std_logic_1164.all;

package smoke_pkg is
  constant DATA_W   : natural := 8;
  constant STATUS_W : natural := 4;
  subtype data_t   is std_logic_vector(DATA_W-1 downto 0);
  subtype status_t is std_logic_vector(STATUS_W-1 downto 0);
end package;
