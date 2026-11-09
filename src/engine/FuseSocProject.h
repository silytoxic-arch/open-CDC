#pragma once

// FuseSoC cores (CAPI2 .core files) as OpenCDC projects.
//
// The given core is the top; the other cores are the .core files found below
// its directory. The import follows FuseSoC:
// - the target: the one asked for, else "synth" if the core has one, else
//   "default"; dependencies use their "default" target;
// - the filesets of the target (and filesets_append), each with its
//   dependencies (VLNV, versions ignored) before the core's own files;
//   conditions such as "tool_vivado ? (files_xilinx)" are evaluated with the
//   flags tool_<tool> (the target's default_tool, else vivado), target_<target>
//   and is_toplevel;
// - file types: systemVerilogSource*, verilogSource*, vhdlSource* (library
//   from logical_name) are sources, include files give include directories,
//   xdc and sdc are constraints; the other types are not read;
// - the target's parameters: vlogdefine are defines, vlogparam and generic
//   are top-level parameters;
// - generators are not run (they are programs), except lowRISC's primgen,
//   whose output (prim_pkg and the abstract primitives selecting a technology
//   library) OpenCDC writes itself.
// What is not read is reported (PRJ-CORE-*).

#include "core/Result.h"
#include "engine/ProjectConfig.h"

#include <filesystem>
#include <string>

namespace opencdc::engine {

/// A .core file.
[[nodiscard]] bool isFuseSocCoreFile(const std::filesystem::path& file);

/// The project of the core `file` for `target` (empty: synth, else default).
/// Errors: PRJ-OPEN, PRJ-CORE-PARSE, PRJ-CORE-TARGET.
[[nodiscard]] Result<ProjectConfig> projectFromCoreFile(const std::filesystem::path& file,
                                                        const std::string& target = {});

} // namespace opencdc::engine
