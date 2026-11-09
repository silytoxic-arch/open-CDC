#pragma once

// Gowin EDA projects (.gprj) as OpenCDC projects.
//
// A .gprj (XML) names the device and lists the project's files, each with a
// type and an enable flag:
//   <File path="src/top.v" type="file.verilog" enable="1"/>
// The synthesis settings are in a JSON file of the project's impl directory:
// impl/<project>_process_config.json (newer Gowin EDA) or
// impl/project_process_config.json. The import takes:
// - the enabled design files: file.verilog (.v, .sv), file.vhdl;
// - the timing constraints: file.sdc;
// - from the settings: the top module (TopModule; empty: found by OpenCDC),
//   the Verilog standard (Verilog_Standard: Vlg_Std_V1995, Vlg_Std_V2001,
//   Vlg_Std_Sysv2017) and the include directories (IncludePath).
// What is not read is reported (PRJ-GPRJ-*): missing files, physical
// constraints (file.cst), analyzer configurations (file.gao), other files.

#include "core/Result.h"
#include "engine/ProjectConfig.h"

#include <filesystem>
#include <string_view>

namespace opencdc::engine {

/// A .gprj.
[[nodiscard]] bool isGowinProjectFile(const std::filesystem::path& file);

/// The project `file` with the settings of its impl directory.
/// Errors: PRJ-OPEN, PRJ-GPRJ-PARSE.
[[nodiscard]] Result<ProjectConfig> projectFromGprjFile(const std::filesystem::path& file);

/// The project text `gprjText` of `gprjFile` and the settings `configJson`
/// (empty: none); paths resolve against the project directory.
[[nodiscard]] Result<ProjectConfig> projectFromGprjText(std::string_view gprjText, std::string_view configJson,
                                                        const std::filesystem::path& gprjFile);

} // namespace opencdc::engine
