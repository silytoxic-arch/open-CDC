#pragma once

// Quartus projects (.qpf/.qsf) as OpenCDC projects.
//
// A Quartus project is a .qpf naming its revisions; a revision's settings are
// a Tcl file (.qsf) of assignments (set_global_assignment -name VERILOG_FILE
// ...), which may source other scripts and include IP file lists (.qip) that
// refer to their own directory as $::quartus(qip_path). The import evaluates
// these scripts as Quartus does, with the Tcl commands they use (set, source,
// file join, join, list, regexp, string, ...), and takes:
// - the design files (VERILOG_FILE, SYSTEMVERILOG_FILE, VHDL_FILE with its
//   -library), in order, through QIP_FILE lists; a Platform Designer system
//   (QSYS_FILE) or IP variation (IP_FILE) by its generated .qip;
// - include directories (SEARCH_PATH, VERILOG_INCLUDE_FILE), Verilog macros
//   (VERILOG_MACRO), top-level parameters (set_parameter), the top entity, the
//   SystemVerilog mode (SYSTEMVERILOG_FILE, VERILOG_INPUT_VERSION);
// - the timing constraints (SDC_FILE).
// What is not read is reported (PRJ-QSF-*): missing files, systems that are
// not generated, file kinds OpenCDC does not read (AHDL, schematics,
// netlists), memory initialization files, Tcl commands not evaluated.

#include "core/Result.h"
#include "engine/ProjectConfig.h"

#include <filesystem>
#include <string_view>

namespace opencdc::engine {

/// A .qpf or a .qsf.
[[nodiscard]] bool isQuartusProjectFile(const std::filesystem::path& file);

/// The revision of a .qpf (its first PROJECT_REVISION, or `revision`), or the
/// .qsf itself. Errors: PRJ-OPEN, PRJ-QSF-PARSE.
[[nodiscard]] Result<ProjectConfig> projectFromQuartusFile(const std::filesystem::path& file,
                                                           const std::string& revision = {});

/// The settings `qsfText` of the revision file `qsfFile` (paths resolve
/// against its directory, the project directory).
[[nodiscard]] Result<ProjectConfig> projectFromQsfText(std::string_view qsfText, const std::filesystem::path& qsfFile);

} // namespace opencdc::engine
