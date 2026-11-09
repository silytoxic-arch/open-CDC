#pragma once

// Verilog file lists (.f): the format of simulators and of many open-source
// IP libraries (Alex Forencich's taxi: taxi_eth_mac_1g_rgmii_fifo.f).
// One entry per line; paths are relative to the list's directory:
//   file.sv           a source (a nested .f is read in its place)
//   -f other.f        a nested list (also -F)
//   +incdir+dir       an include directory (several separated by '+')
//   +define+A=1+B     macros
// Comments (// and #, to the end of the line) and blank lines are skipped; other options (-v, -y,
// +libext+, ...) are reported as ignored.

#include "frontend/Frontend.h"

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace opencdc::engine {

struct FileListResult {
    std::vector<std::filesystem::path> sources;
    std::vector<std::filesystem::path> includeDirs;
    std::vector<frontend::MacroDefine> defines;
    std::vector<std::string> ignored; ///< options not read, "file:line: text"
    std::string error;                ///< a list that cannot be read (empty: none)
};

/// A file list path (.f, any case).
[[nodiscard]] bool isFileList(const std::filesystem::path& file);

/// The sources of `list` and of the lists it names. Files already in `seen`
/// (canonical paths: a library listed by several lists) are skipped; the
/// sources returned are added to it.
[[nodiscard]] FileListResult readFileList(const std::filesystem::path& list, std::set<std::filesystem::path>& seen);

} // namespace opencdc::engine
