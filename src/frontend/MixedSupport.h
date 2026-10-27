#pragma once
// Internal interface between the language frontends for mixed-language
// designs (not part of the public frontend API).

#include "frontend/Frontend.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace opencdc::frontend::detail {

/// Note attached to "unknown module" errors.
inline constexpr const char* kUnknownModuleHint =
    "if it is a vendor IP core: add its Vivado configuration <ip>.xci to the sources (FIFO Generator, "
    "AXI4-Stream Data FIFO, Clocking Wizard, Block Memory Generator, CORDIC and other one-clock datapath IP, "
    "the DDR2/DDR3 memory interface, AXI GPIO/IIC/Timer/UART/Quad SPI, Slice/Concat/Constant, ILA and VIO are "
    "modelled from it), or the HDL the IP generates (Vivado: "
    "<ip>/synth/<ip>.v or .vhd, or its simulation netlist; Quartus: the megafunction's .v/.vhd), or set the "
    "project option 'unknownModules' to 'blackbox' to analyze the rest of the design around it";

struct VerilogMixedOptions {
    /// Definitions provided by the other language: instances of them are kept
    /// as unresolved children (parameters and port actuals as written).
    std::set<std::string> foreignModules;
    /// Top modules; empty: inferred by slang.
    std::vector<std::string> tops;
    /// Output: non-local parameter names of every module definition.
    std::map<std::string, std::vector<std::string>>* moduleParams = nullptr;
    /// Built-in vendor primitive models: connections to ports they do not
    /// model are reported as information (FE-PRIMITIVE-PORT), not errors.
    std::set<std::string> lenientModules;
    /// Unknown modules are black boxes (warning FE-BLACKBOX-MODULE), not errors.
    bool blackBoxUnknown = false;
};

/// The slang frontend with mixed-language options.
[[nodiscard]] Result<FrontendOutput> elaborateVerilog(const FrontendInput& input, DiagnosticSink& sink,
                                                      const VerilogMixedOptions* mixed);

} // namespace opencdc::frontend::detail
