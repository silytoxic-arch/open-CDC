-- OpenCDC: component declarations of the modelled Intel LPM cells
-- (library lpm; use lpm.lpm_components.all;).
-- OPENCDC-LIBRARIES: lpm
library ieee;
use ieee.std_logic_1164.all;

package lpm_components is
  component lpm_mult
    generic (lpm_widtha : natural; lpm_widthb : natural; lpm_widthp : natural; lpm_widths : natural := 1;
             lpm_representation : string := "UNSIGNED"; lpm_pipeline : natural := 0;
             lpm_type : string := "LPM_MULT"; lpm_hint : string := "UNUSED");
    port (dataa : in std_logic_vector(lpm_widtha-1 downto 0); datab : in std_logic_vector(lpm_widthb-1 downto 0);
          sum : in std_logic_vector(lpm_widths-1 downto 0) := (others => '0');
          aclr : in std_logic := '0'; sclr : in std_logic := '0'; clock : in std_logic := '0';
          clken : in std_logic := '1'; result : out std_logic_vector(lpm_widthp-1 downto 0));
  end component;
  component lpm_divide
    generic (lpm_widthn : natural; lpm_widthd : natural; lpm_nrepresentation : string := "UNSIGNED";
             lpm_drepresentation : string := "UNSIGNED"; lpm_pipeline : natural := 0;
             lpm_remainderpositive : string := "TRUE"; maximize_speed : natural := 5;
             lpm_type : string := "LPM_DIVIDE"; lpm_hint : string := "UNUSED");
    port (numer : in std_logic_vector(lpm_widthn-1 downto 0); denom : in std_logic_vector(lpm_widthd-1 downto 0);
          aclr : in std_logic := '0'; clock : in std_logic := '0'; clken : in std_logic := '1';
          quotient : out std_logic_vector(lpm_widthn-1 downto 0); remain : out std_logic_vector(lpm_widthd-1 downto 0));
  end component;
end package;
