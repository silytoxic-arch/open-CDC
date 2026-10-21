#pragma once

#include "core/Diagnostic.h"
#include "core/Result.h"
#include "frontend/Frontend.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace opencdc::engine {

/// What to assume for clock pairs whose relation cannot be determined
/// (independent sources, no constraint). See docs/architecture.md §4.3.
enum class UnknownClockPolicy {
    Report,      ///< analyze as asynchronous and flag every crossing for review (CDC-010)
    AssumeAsync, ///< analyze as asynchronous, no extra review flag
    AssumeSync,  ///< treat as synchronous (crossings between them are not analyzed)
};

[[nodiscard]] std::string_view toString(UnknownClockPolicy p) noexcept;

struct AnalysisOptions {
    UnknownClockPolicy unknownClockRelation = UnknownClockPolicy::Report;
    std::uint32_t minSyncStages = 2; ///< flop chains shorter than this are reported by CDC-002
    std::uint32_t reconvergenceDepth = 2; ///< register levels searched after synchronizers (CDC-005)
};

/// In-memory form of an OpenCDC project file (*.ocdc.json).
///
/// File format (version 1). Relative paths are relative to the project file:
/// {
///   "format":      "opencdc-project",
///   "version":     1,
///   "name":        "my_design",
///   "top":         "top",                       (optional)
///   "language":    "verilog-2001",              (optional; verilog-2001 | verilog-2005 | systemverilog-2017; default verilog-2001, or systemverilog-2017 when a source is .sv/.svh)
///   "sources":     ["rtl/top.v", "rtl/sync.v",
///                   {"file": "rtl/olo_base_cc_bits.vhd", "library": "olo"}],
///                  (.vhd/.vhdl files are VHDL; a source may name its VHDL library, default "work")
///   "includeDirs": ["rtl/include"],             (optional)
///   "defines":     {"WIDTH": "8", "USE_FIFO": null},  (optional; null = define without value)
///   "parameters":  {"DEPTH": 16, "MODE": "\"FAST\""},  (optional; top-level parameter / generic overrides)
///   "constraints": ["constraints/clocks.sdc"],  (optional; SDC/XDC subset)
///   "waivers":     ["waivers.json"],            (optional; see engine/WaiverFile.h)
///   "options":     {"unknownClockRelation": "report",   (optional; report | assume_async | assume_sync)
///                   "minSyncStages": 2,                 (optional; 1..8)
///                   "reconvergenceDepth": 2}            (optional; 0..8)
/// }
struct ProjectConfig {
    std::filesystem::path projectFile; ///< absolute path of the .ocdc.json, empty if built in memory
    std::string name;
    std::string top;
    frontend::LanguageStd language = frontend::LanguageStd::Verilog2001;
    std::vector<std::filesystem::path> sources;     ///< absolute
    std::vector<std::string> sourceLibraries;       ///< VHDL library per source ("" = work), parallel to sources
    std::vector<std::filesystem::path> includeDirs; ///< absolute
    std::vector<frontend::MacroDefine> defines;
    /// Top-level parameter (Verilog) / generic (VHDL) overrides, value in HDL syntax.
    std::vector<std::pair<std::string, std::string>> parameters;
    std::vector<std::filesystem::path> constraints; ///< absolute
    std::vector<std::filesystem::path> waivers;     ///< absolute
    AnalysisOptions options;

    [[nodiscard]] frontend::FrontendInput toFrontendInput() const;
    /// HDLs of the sources: "verilog-2001", "vhdl" or "verilog-2001 + vhdl".
    [[nodiscard]] std::string languageSummary() const;
};

inline constexpr const char* kProjectFormatName = "opencdc-project";
inline constexpr int kProjectFormatVersion = 1;

/// Reads and validates a project file. Errors carry codes PRJ-*.
[[nodiscard]] Result<ProjectConfig> loadProjectFile(const std::filesystem::path& file);

/// Parses project JSON text; `baseDir` resolves relative paths.
[[nodiscard]] Result<ProjectConfig> parseProjectJson(std::string_view jsonText,
                                                     const std::filesystem::path& baseDir);

/// Project JSON text (format version 1) for `cfg`. Paths are written relative
/// to `baseDir` when they can be, with '/' separators; parseProjectJson(text,
/// baseDir) gives back the same configuration.
[[nodiscard]] std::string projectToJson(const ProjectConfig& cfg, const std::filesystem::path& baseDir);

/// Writes `cfg` to `file` (relative paths against the file's directory). The
/// file is replaced only once the new text is completely written. Error code PRJ-WRITE.
[[nodiscard]] std::optional<Diagnostic> saveProjectFile(const ProjectConfig& cfg, const std::filesystem::path& file);

} // namespace opencdc::engine
