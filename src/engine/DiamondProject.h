#pragma once

// Lattice Diamond projects (.ldf) as OpenCDC projects.
//
// A .ldf (XML, <BaliProject>) names the device and holds implementations; the
// default one (default_implementation) is imported:
//   <Implementation title="impl1" dir="impl1" synthesis="lse">
//     <Options def_top="top" top="top"/>
//     <Source name="../rtl/top.v" type="Verilog" type_short="Verilog"><Options top_module="top"/></Source>
//     <Source name="pll.ipx" type="IPX_Module" type_short="IPX"><Options/></Source>
//     <Source name="top.lpf" type="Logic Preference" type_short="LPF"><Options/></Source>
// The import takes:
// - the design files in order (Verilog, SystemVerilog, VHDL with the library
//   of <Options lib="..."/>), not those marked excluded="TRUE";
// - an IPexpress module (.ipx) by the HDL it generated (its top_level_verilog
//   or top_level_vhdl file);
// - the constraints: preference files (.lpf, see constraints/Lpf.h) and
//   Synplify constraints (.sdc, .fdc);
// - the top: the implementation's top, else its default top, else the source
//   marked top_module.
// What is not read is reported (PRJ-LDF-*).

#include "core/Result.h"
#include "engine/ProjectConfig.h"

#include <filesystem>
#include <string_view>

namespace opencdc::engine {

/// A .ldf.
[[nodiscard]] bool isDiamondProjectFile(const std::filesystem::path& file);

/// The default implementation of the project `file`.
/// Errors: PRJ-OPEN, PRJ-LDF-PARSE.
[[nodiscard]] Result<ProjectConfig> projectFromLdfFile(const std::filesystem::path& file);

/// The same from the text of `ldfFile` (paths resolve against its directory).
[[nodiscard]] Result<ProjectConfig> projectFromLdfText(std::string_view text, const std::filesystem::path& ldfFile);

} // namespace opencdc::engine
