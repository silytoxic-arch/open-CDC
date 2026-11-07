#pragma once

// Vivado projects (.xpr) as OpenCDC projects.
//
// A .xpr lists the project's file sets: the design sources of the current
// synthesis run (HDL, IP configurations .xci/.xcix, block designs .bd), its
// constraint set, and per file whether it is used in synthesis, its VHDL
// library and its constraint scope. The import takes what Vivado synthesizes:
// - the sources of the current synthesis run's source set used in synthesis
//   (not disabled), in the project's order; IP of their own file sets
//   (BlockSrcs) unless they belong to a listed block design;
// - Verilog headers (.vh, .svh, .h, global includes) are no sources: their
//   directories become include directories, as Vivado finds them by name;
// - the top module, Verilog defines, include directories and generics of the
//   source set;
// - the constraint files of the run's constraint set (XDC, SDC, Tcl), scoped to
//   their module when Vivado scopes them (SCOPED_TO_REF);
// - the IP repositories of the project.
// Files that are missing, of a kind OpenCDC does not read (netlists, memory
// initialization files) or scoped to a cell only are reported (PRJ-XPR-*).

#include "core/Diagnostic.h"
#include "core/Result.h"
#include "engine/ProjectConfig.h"

#include <filesystem>
#include <string_view>

namespace opencdc::engine {

[[nodiscard]] bool isVivadoProjectFile(const std::filesystem::path& file);

/// The project in `xprText`; `xprFile` is its path (variables such as $PPRDIR
/// and $PSRCDIR are resolved against it). Error PRJ-XPR-PARSE when the text is
/// not a Vivado project or has no design sources.
[[nodiscard]] Result<ProjectConfig> projectFromXprText(std::string_view xprText, const std::filesystem::path& xprFile);
[[nodiscard]] Result<ProjectConfig> projectFromXprFile(const std::filesystem::path& xprFile);

} // namespace opencdc::engine
