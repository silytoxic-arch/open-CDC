#pragma once
// Cross-check of the clock model against Vivado's own clock list.
//
// Vivado derives the clocks of MMCMs, PLLs, BUFR/BUFGCE_DIV and GT
// transceivers itself and lists them with `report_clocks`. Reading that
// report back lets OpenCDC compare the periods it computed from the vendor
// models with the ones the implementation tool uses, and points out clocks
// that only one of the two knows about.

#include "clock/ClockModel.h"
#include "core/Diagnostic.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opencdc::engine {

/// One row of the clock table of `report_clocks`.
struct VivadoClock {
    std::string name;
    double periodNs = 0.0;
    std::string attributes;           ///< "P,G,A" etc. (empty in the old report format)
    std::vector<std::string> sources; ///< pins/ports, as Vivado prints them
    std::uint32_t line = 0;           ///< 1-based line in the report
};

/// Parses the text of a `report_clocks` output. Both the current table
/// ("Clock  Period(ns)  Waveform(ns)  Attributes  Sources") and the older one
/// ("Clock  Waveform(ns)  Period(ns)  Frequency(MHz)", sources taken from the
/// "Generated Clock" details) are understood. Returns nullopt and sets
/// `error` when the text holds no clock table.
[[nodiscard]] std::optional<std::vector<VivadoClock>> parseVivadoClockReport(std::string_view text,
                                                                             std::string& error);

struct VivadoClockCheck {
    std::size_t vivadoClocks = 0; ///< rows read from all reports
    std::size_t matched = 0;      ///< rows matched to a clock of the model
    std::size_t mismatched = 0;   ///< matched rows whose period differs (> 0.1 %)
    std::size_t unknownInModel = 0; ///< matched rows for which OpenCDC has no period
    std::size_t unmatched = 0;    ///< rows without a counterpart in the model
    std::size_t missing = 0;      ///< model clocks without a counterpart in the report
};

/// Compares the clock model with Vivado's clocks. Clocks are matched by
/// source (port, vendor pin "inst/CLKOUT0", register pin "cnt_reg[2]/Q";
/// '.' and '/' are equivalent, case is ignored) or else by name. Reports
/// VIV-PERIOD-MISMATCH (warning) for differing periods, and VIV-PERIOD-UNKNOWN,
/// VIV-CLOCK-UNMATCHED and VIV-CLOCK-MISSING (info) for the rest. `top` is the
/// top instance name (stripped from hierarchical names).
VivadoClockCheck checkVivadoClocks(const clock::ClockModel& model, const std::string& top,
                                   const std::vector<VivadoClock>& vivado, const std::string& reportFile,
                                   DiagnosticSink& sink);

/// Reads and checks one report file; VIV-REPORT-READ (error) when the file
/// cannot be read or holds no clock table.
std::optional<VivadoClockCheck> checkVivadoClockReport(const clock::ClockModel& model, const std::string& top,
                                                       const std::filesystem::path& file, DiagnosticSink& sink);

} // namespace opencdc::engine
