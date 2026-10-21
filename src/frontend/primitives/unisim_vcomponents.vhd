-- OpenCDC: component declarations of the modelled Xilinx UNISIM primitives
-- (library unisim; use unisim.vcomponents.all;). The models are Verilog
-- (xilinx_unisim.v) and are bound through the mixed-language linker.
library ieee;
use ieee.std_logic_1164.all;

package vcomponents is
  component FDRE generic (INIT : bit := '0'; IS_C_INVERTED : bit := '0'; IS_D_INVERTED : bit := '0';
                          IS_R_INVERTED : bit := '0');
    port (Q : out std_ulogic; C : in std_ulogic; CE : in std_ulogic; D : in std_ulogic; R : in std_ulogic);
  end component;
  component FDSE generic (INIT : bit := '1'; IS_C_INVERTED : bit := '0'; IS_D_INVERTED : bit := '0';
                          IS_S_INVERTED : bit := '0');
    port (Q : out std_ulogic; C : in std_ulogic; CE : in std_ulogic; D : in std_ulogic; S : in std_ulogic);
  end component;
  component FDCE generic (INIT : bit := '0'; IS_C_INVERTED : bit := '0'; IS_D_INVERTED : bit := '0';
                          IS_CLR_INVERTED : bit := '0');
    port (Q : out std_ulogic; C : in std_ulogic; CE : in std_ulogic; CLR : in std_ulogic; D : in std_ulogic);
  end component;
  component FDPE generic (INIT : bit := '1'; IS_C_INVERTED : bit := '0'; IS_D_INVERTED : bit := '0';
                          IS_PRE_INVERTED : bit := '0');
    port (Q : out std_ulogic; C : in std_ulogic; CE : in std_ulogic; D : in std_ulogic; PRE : in std_ulogic);
  end component;
  component FD generic (INIT : bit := '0');
    port (Q : out std_ulogic; C : in std_ulogic; D : in std_ulogic);
  end component;
  component FDE generic (INIT : bit := '0');
    port (Q : out std_ulogic; C : in std_ulogic; CE : in std_ulogic; D : in std_ulogic);
  end component;
  component FDR generic (INIT : bit := '0');
    port (Q : out std_ulogic; C : in std_ulogic; D : in std_ulogic; R : in std_ulogic);
  end component;
  component FDS generic (INIT : bit := '1');
    port (Q : out std_ulogic; C : in std_ulogic; D : in std_ulogic; S : in std_ulogic);
  end component;
  component FDC generic (INIT : bit := '0');
    port (Q : out std_ulogic; C : in std_ulogic; CLR : in std_ulogic; D : in std_ulogic);
  end component;
  component FDP generic (INIT : bit := '1');
    port (Q : out std_ulogic; C : in std_ulogic; D : in std_ulogic; PRE : in std_ulogic);
  end component;
  component FDCPE generic (INIT : bit := '0');
    port (Q : out std_ulogic; C : in std_ulogic; CE : in std_ulogic; CLR : in std_ulogic; D : in std_ulogic;
          PRE : in std_ulogic);
  end component;
  component LDCE generic (INIT : bit := '0'; IS_CLR_INVERTED : bit := '0'; IS_G_INVERTED : bit := '0');
    port (Q : out std_ulogic; CLR : in std_ulogic; D : in std_ulogic; G : in std_ulogic; GE : in std_ulogic);
  end component;
  component LDPE generic (INIT : bit := '1'; IS_PRE_INVERTED : bit := '0'; IS_G_INVERTED : bit := '0');
    port (Q : out std_ulogic; D : in std_ulogic; G : in std_ulogic; GE : in std_ulogic; PRE : in std_ulogic);
  end component;
  component BUFG port (O : out std_ulogic; I : in std_ulogic); end component;
  component BUFGCE generic (CE_TYPE : string := "SYNC"; IS_CE_INVERTED : bit := '0'; IS_I_INVERTED : bit := '0';
                            SIM_DEVICE : string := "ULTRASCALE");
    port (O : out std_ulogic; CE : in std_ulogic; I : in std_ulogic);
  end component;
  component BUFGCE_1 port (O : out std_ulogic; CE : in std_ulogic; I : in std_ulogic); end component;
  component BUFGMUX generic (CLK_SEL_TYPE : string := "SYNC");
    port (O : out std_ulogic; I0 : in std_ulogic; I1 : in std_ulogic; S : in std_ulogic);
  end component;
  component BUFGMUX_CTRL port (O : out std_ulogic; I0 : in std_ulogic; I1 : in std_ulogic; S : in std_ulogic);
  end component;
  component BUFGCTRL generic (INIT_OUT : integer := 0; PRESELECT_I0 : boolean := false;
                              PRESELECT_I1 : boolean := false);
    port (O : out std_ulogic; CE0 : in std_ulogic; CE1 : in std_ulogic; I0 : in std_ulogic; I1 : in std_ulogic;
          IGNORE0 : in std_ulogic; IGNORE1 : in std_ulogic; S0 : in std_ulogic; S1 : in std_ulogic);
  end component;
  component BUFH port (O : out std_ulogic; I : in std_ulogic); end component;
  component BUFHCE generic (INIT_OUT : integer := 0; CE_TYPE : string := "SYNC");
    port (O : out std_ulogic; CE : in std_ulogic; I : in std_ulogic);
  end component;
  component BUFR generic (BUFR_DIVIDE : string := "BYPASS"; SIM_DEVICE : string := "7SERIES");
    port (O : out std_ulogic; CE : in std_ulogic; CLR : in std_ulogic; I : in std_ulogic);
  end component;
  component BUFIO port (O : out std_ulogic; I : in std_ulogic); end component;
  component IBUF generic (IBUF_LOW_PWR : boolean := true; IOSTANDARD : string := "DEFAULT");
    port (O : out std_ulogic; I : in std_ulogic);
  end component;
  component IBUFG generic (IBUF_LOW_PWR : boolean := true; IOSTANDARD : string := "DEFAULT");
    port (O : out std_ulogic; I : in std_ulogic);
  end component;
  component IBUFDS generic (DIFF_TERM : boolean := false; IBUF_LOW_PWR : boolean := true;
                            IOSTANDARD : string := "DEFAULT");
    port (O : out std_ulogic; I : in std_ulogic; IB : in std_ulogic);
  end component;
  component IBUFGDS generic (DIFF_TERM : boolean := false; IBUF_LOW_PWR : boolean := true;
                             IOSTANDARD : string := "DEFAULT");
    port (O : out std_ulogic; I : in std_ulogic; IB : in std_ulogic);
  end component;
  component OBUF generic (DRIVE : integer := 12; IOSTANDARD : string := "DEFAULT"; SLEW : string := "SLOW");
    port (O : out std_ulogic; I : in std_ulogic);
  end component;
  component OBUFT generic (DRIVE : integer := 12; IOSTANDARD : string := "DEFAULT"; SLEW : string := "SLOW");
    port (O : out std_ulogic; I : in std_ulogic; T : in std_ulogic);
  end component;
  component OBUFDS generic (IOSTANDARD : string := "DEFAULT"; SLEW : string := "SLOW");
    port (O : out std_ulogic; OB : out std_ulogic; I : in std_ulogic);
  end component;
  component SRL16E generic (INIT : bit_vector(15 downto 0) := x"0000"; IS_CLK_INVERTED : bit := '0');
    port (Q : out std_ulogic; A0 : in std_ulogic; A1 : in std_ulogic; A2 : in std_ulogic; A3 : in std_ulogic;
          CE : in std_ulogic; CLK : in std_ulogic; D : in std_ulogic);
  end component;
  component SRLC32E generic (INIT : bit_vector(31 downto 0) := x"00000000"; IS_CLK_INVERTED : bit := '0');
    port (Q : out std_ulogic; Q31 : out std_ulogic; A : in std_logic_vector(4 downto 0); CE : in std_ulogic;
          CLK : in std_ulogic; D : in std_ulogic);
  end component;
  component LUT1 generic (INIT : bit_vector(1 downto 0) := "00");
    port (O : out std_ulogic; I0 : in std_ulogic);
  end component;
  component LUT2 generic (INIT : bit_vector(3 downto 0) := x"0");
    port (O : out std_ulogic; I0 : in std_ulogic; I1 : in std_ulogic);
  end component;
  component LUT3 generic (INIT : bit_vector(7 downto 0) := x"00");
    port (O : out std_ulogic; I0 : in std_ulogic; I1 : in std_ulogic; I2 : in std_ulogic);
  end component;
  component LUT4 generic (INIT : bit_vector(15 downto 0) := x"0000");
    port (O : out std_ulogic; I0 : in std_ulogic; I1 : in std_ulogic; I2 : in std_ulogic; I3 : in std_ulogic);
  end component;
  component LUT5 generic (INIT : bit_vector(31 downto 0) := x"00000000");
    port (O : out std_ulogic; I0 : in std_ulogic; I1 : in std_ulogic; I2 : in std_ulogic; I3 : in std_ulogic;
          I4 : in std_ulogic);
  end component;
  component LUT6 generic (INIT : bit_vector(63 downto 0) := x"0000000000000000");
    port (O : out std_ulogic; I0 : in std_ulogic; I1 : in std_ulogic; I2 : in std_ulogic; I3 : in std_ulogic;
          I4 : in std_ulogic; I5 : in std_ulogic);
  end component;
  component MUXF7 port (O : out std_ulogic; I0 : in std_ulogic; I1 : in std_ulogic; S : in std_ulogic);
  end component;
  component MUXF8 port (O : out std_ulogic; I0 : in std_ulogic; I1 : in std_ulogic; S : in std_ulogic);
  end component;
  component VCC port (P : out std_ulogic); end component;
  component GND port (G : out std_ulogic); end component;
  -- clock generator: generics and ports are taken from the instance (bound to a generated model)
  component MMCME2_BASE port (CLKIN1 : in std_ulogic); end component;
  -- clock generator: generics and ports are taken from the instance (bound to a generated model)
  component MMCME2_ADV port (CLKIN1 : in std_ulogic); end component;
  -- clock generator: generics and ports are taken from the instance (bound to a generated model)
  component MMCME3_BASE port (CLKIN1 : in std_ulogic); end component;
  -- clock generator: generics and ports are taken from the instance (bound to a generated model)
  component MMCME3_ADV port (CLKIN1 : in std_ulogic); end component;
  -- clock generator: generics and ports are taken from the instance (bound to a generated model)
  component MMCME4_BASE port (CLKIN1 : in std_ulogic); end component;
  -- clock generator: generics and ports are taken from the instance (bound to a generated model)
  component MMCME4_ADV port (CLKIN1 : in std_ulogic); end component;
  -- clock generator: generics and ports are taken from the instance (bound to a generated model)
  component PLLE2_BASE port (CLKIN1 : in std_ulogic); end component;
  -- clock generator: generics and ports are taken from the instance (bound to a generated model)
  component PLLE2_ADV port (CLKIN1 : in std_ulogic); end component;
  -- clock generator: generics and ports are taken from the instance (bound to a generated model)
  component PLLE3_BASE port (CLKIN : in std_ulogic); end component;
  -- clock generator: generics and ports are taken from the instance (bound to a generated model)
  component PLLE3_ADV port (CLKIN : in std_ulogic); end component;
  -- clock generator: generics and ports are taken from the instance (bound to a generated model)
  component PLLE4_BASE port (CLKIN : in std_ulogic); end component;
  -- clock generator: generics and ports are taken from the instance (bound to a generated model)
  component PLLE4_ADV port (CLKIN : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component IDDR port (C : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component IDDRE1 port (C : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component ODDR port (C : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component ODDRE1 port (C : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component IDELAYE2 port (C : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component ODELAYE2 port (C : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component IDELAYE3 port (CLK : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component ODELAYE3 port (CLK : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component IDELAYCTRL port (REFCLK : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component RAMB18E1 port (CLKARDCLK : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component RAMB36E1 port (CLKARDCLK : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component RAMB18E2 port (CLKARDCLK : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component RAMB36E2 port (CLKARDCLK : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component BUFGCE_DIV port (I : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component ISERDESE2 port (CLK : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component OSERDESE2 port (CLK : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component ISERDESE3 port (CLK : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component OSERDESE3 port (CLK : in std_ulogic); end component;
  -- generics and ports are taken from the instance (bound to the Verilog model)
  component URAM288 port (CLK : in std_ulogic); end component;
end package;
