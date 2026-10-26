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
// (outputs in the clk domain). Both the XML (IP-XACT, Vivado up to 2020.1) and
// the JSON (Vivado 2020.2 and later) formats are read.

#include "core/Result.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace opencdc::frontend {

struct XciModel {
    std::string module;  ///< the IP instance name (the module the design instantiates)
    std::string ip;      ///< "xilinx.com:ip:fifo_generator:13.2"
    std::string summary; ///< "independent clocks, 32 x 16, first-word fall-through, native"
    std::string verilog; ///< model source
    std::vector<std::string> notes; ///< e.g. outputs driven with a constant
};

/// True for .xci files.
[[nodiscard]] bool isXciFile(const std::filesystem::path& file);

/// Model of the IP configured by an .xci file's text. The error is a
/// warning (FE-XCI-UNSUPPORTED or FE-XCI-PARSE) when the IP is not modelled.
[[nodiscard]] Result<XciModel> modelFromXciText(std::string_view text);

/// As above, reading the file (locations of errors point to it).
[[nodiscard]] Result<XciModel> modelFromXci(const std::filesystem::path& file);

} // namespace opencdc::frontend
