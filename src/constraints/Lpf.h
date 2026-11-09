#pragma once

// Lattice preference files (.lpf, Diamond): the timing preferences that matter
// for clock analysis, as SDC commands that the SDC reader interprets.
//
//   FREQUENCY PORT "clk" 25 MHZ;             create_clock on the port
//   FREQUENCY NET "u/clk" 100 MHZ;           create_clock on the net
//   PERIOD PORT|NET "clk" 10 NS;             likewise
//   BLOCK PATH FROM CLKNET "a" TO CLKNET "b";
//                                            set_false_path between the clocks of the
//                                            nets (also PORT, CELL, NET endpoints)
//   BLOCK INTERCLOCKDOMAIN PATHS;            every pair of clocks is asynchronous
//
// The other timing preferences (MULTICYCLE, MAXDELAY, INPUT_SETUP, BLOCK NET,
// ...) are listed in one CON-LPF-IGNORED note; physical preferences (LOCATE,
// IOBUF, SYSCONFIG, ...) are skipped silently. BLOCK RESETPATHS, ASYNCPATHS
// and JTAGPATHS exclude paths that CDC analysis does not time either.
// Each SDC command is written on the line of its preference, so diagnostics
// name the preference's line.

#include "core/Diagnostic.h"

#include <string>
#include <string_view>

namespace opencdc::constraints {

/// The SDC text for the preferences in `text` of `fileName`.
/// Diagnostics: CON-LPF-IGNORED (info), CON-LPF-PARSE (warning).
[[nodiscard]] std::string lpfToSdc(std::string_view text, const std::string& fileName, DiagnosticSink& sink);

/// A Lattice preference file (.lpf, any case).
[[nodiscard]] bool isLpfFile(std::string_view fileName);

} // namespace opencdc::constraints
