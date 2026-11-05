#pragma once

// Vivado block designs (.bd) as Verilog.
//
// A .bd (JSON, Vivado 2014+) lists cells (IP, module references and
// hierarchies), the nets between their ports and the interface nets between
// their bus interfaces. Vivado generates a Verilog module from it; OpenCDC
// does the same: one module named after the design, every cell an instance
// under its name, a module per hierarchical cell (so instance paths are
// Vivado's), interface nets expanded to their signals through the port maps of the
// cells' component files (ip/<cell>/<cell>.xml, or the boundary of a JSON
// .xci). The IP cells are then modelled from their .xci as any other IP.

#include "core/Result.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace opencdc::frontend {

struct BlockDesign {
    std::string name;    ///< the design (and module) name
    std::string verilog; ///< the module
    /// The configuration (.xci) of every IP cell, to model them.
    std::vector<std::filesystem::path> ipFiles;
    std::vector<std::string> notes;
    std::size_t cells = 0;       ///< leaf cells
    std::size_t hierarchies = 0; ///< hierarchical cells (a module each)
};

[[nodiscard]] bool isBlockDesignFile(const std::filesystem::path& file);

/// The module of a block design; the component files of the cells are read
/// from `dir`/ip/<cell>/. Errors: FE-BD-PARSE (not a block design),
/// FE-BD-CELL (a cell whose ports are not known).
[[nodiscard]] Result<BlockDesign> blockDesignFromText(std::string_view text, const std::filesystem::path& dir);
[[nodiscard]] Result<BlockDesign> blockDesignFromFile(const std::filesystem::path& file);

} // namespace opencdc::frontend
