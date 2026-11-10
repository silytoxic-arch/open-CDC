#pragma once

// nextpnr constraint files: the clock constraints in them, as SDC commands that the
// SDC reader interprets.
//
//   .pcf (iCE40 physical constraints)
//     set_frequency <net> <MHz>            create_clock on the net (or the port)
//     set_io, set_location, ...            placement: skipped
//   .py (a script for nextpnr's --pre-pack, as in the CSI-2 receiver's iCEBreaker test)
//     ctx.addClock("<net>", <MHz>)         create_clock on the net
//     other statements                     skipped, counted in one CON-NEXTPNR-IGNORED note
//
// Net names are Yosys' hierarchical names ("csi_rx_i.dphy_clk"): '.' separates the levels.
// Each SDC command is written on the line of its constraint, so diagnostics name its line.

#include "core/Diagnostic.h"

#include <string>
#include <string_view>

namespace opencdc::constraints {

/// An iCE40 physical constraint file (.pcf, any case).
[[nodiscard]] bool isPcfFile(std::string_view fileName);
/// A nextpnr pre-pack Python script (.py).
[[nodiscard]] bool isNextpnrScript(std::string_view fileName);

/// The SDC text for the clock constraints of a .pcf file.
/// Diagnostics: CON-NEXTPNR-PARSE (warning) for a malformed set_frequency.
[[nodiscard]] std::string pcfToSdc(std::string_view text, const std::string& fileName, DiagnosticSink& sink);
/// The SDC text for the ctx.addClock calls of a nextpnr pre-pack script.
/// Diagnostics: CON-NEXTPNR-PARSE (warning), CON-NEXTPNR-IGNORED (info).
[[nodiscard]] std::string nextpnrScriptToSdc(std::string_view text, const std::string& fileName, DiagnosticSink& sink);

} // namespace opencdc::constraints
