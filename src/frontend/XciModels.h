#pragma once

// Vivado IP configuration files (.xci) as behavioural Verilog models.
//
// A project may list the .xci of an IP instead of its generated HDL. The
// models capture what matters for CDC analysis: which ports belong to which
// clock, and how the IP crosses between them (Gray-coded pointers through
// synchronizer chains, reset synchronizers). They are not functional models
// of the IP.
//
// Supported: FIFO Generator (fifo_generator 12.x/13.x; native and AXI-Stream
// interfaces, common or independent clocks) and AXI4-Stream Data FIFO
// (axis_data_fifo 1.x/2.x), and the debug cores ILA (inputs only) and VIO
// (outputs in the clk domain), Clocking Wizard, Block Memory Generator,
// one-clock datapath IP, the 7-series memory interface (mig_7series,
// DDR2/DDR3), AXI GPIO/IIC/Timer/UART/Quad SPI and the block design Slice,
// Concat and Constant. Next to an XML .xci, the IP's component file
// (<instance>.xml) gives the port list. Both the XML (IP-XACT, Vivado up to 2020.1) and
// the JSON (Vivado 2020.2 and later) formats are read.

#include "core/Result.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace opencdc::frontend {

struct XciModel {
    std::string module;  ///< the IP instance name (the module the design instantiates)
    std::string ip;      ///< "xilinx.com:ip:fifo_generator:13.2"
    std::string summary; ///< "independent clocks, 32 x 16, first-word fall-through, native"
    std::string verilog; ///< model source
    std::vector<std::string> notes; ///< e.g. outputs driven with a constant
    /// HDL files of the IP itself (an IP found in an IP repository: its
    /// synthesis file set; may contain further .xci), with their VHDL library.
    std::vector<std::filesystem::path> extraSources;
    std::vector<std::string> extraLibraries;
};

/// A port of an IP instance.
struct XciPort {
    std::string name;
    bool output = false;
    bool inout = false;
    std::optional<std::pair<long long, long long>> range; ///< [left:right], absent for scalars
};

/// The ports of an IP as a VHDL component declaration in the design gives them.
struct XciComponent {
    std::vector<XciPort> ports;
    std::string where; ///< "file:line" of the declaration
};

/// Where IP that OpenCDC does not model is looked up: IP repositories
/// (directories searched for the IP's component.xml, as Vivado's
/// ip_repo_paths). VHDL component declarations of the design give the ports
/// of an XML .xci that has no component file next to it.
struct XciContext {
    std::vector<std::filesystem::path> ipRepositories;
    std::map<std::string, XciComponent> vhdlComponents; ///< lower-case component name -> ports
};

/// Component declarations in a VHDL design file whose port widths are
/// literal (std_logic, std_logic_vector(8 downto 0), ...), by lower-case name.
/// A component with a width that depends on a generic or a constant is left out.
[[nodiscard]] std::map<std::string, XciComponent> vhdlComponentPorts(std::string_view text, const std::string& fileName);

/// True for .xci files.
[[nodiscard]] bool isXciFile(const std::filesystem::path& file);

/// Model of the IP configured by an .xci file's text. The error is a
/// warning (FE-XCI-UNSUPPORTED or FE-XCI-PARSE) when the IP is not modelled.
[[nodiscard]] Result<XciModel> modelFromXciText(std::string_view text);
/// As above; files the configuration refers to (the memory interface's
/// XML_INPUT_FILE, e.g. mig_a.prj) are looked up in `dir`.
[[nodiscard]] Result<XciModel> modelFromXciText(std::string_view text, const std::filesystem::path& dir);

/// As above, reading the file (locations of errors point to it).
[[nodiscard]] Result<XciModel> modelFromXci(const std::filesystem::path& file);
/// As above; an IP without a model of its own is looked up in the IP
/// repositories: its HDL (synthesis file set) is added and a wrapper with the
/// instance name passes the configured parameters (MODELPARAM values).
[[nodiscard]] Result<XciModel> modelFromXci(const std::filesystem::path& file, const XciContext& context);

} // namespace opencdc::frontend
