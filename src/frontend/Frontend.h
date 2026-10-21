#pragma once

#include "core/Diagnostic.h"
#include "core/Result.h"
#include "core/SourceLocation.h"
#include "ir/Design.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opencdc::frontend {

/// HDL language standard used to parse the sources.
enum class LanguageStd {
    Verilog2001,        ///< IEEE 1364-2001 keywords (default for FPGA RTL)
    Verilog2005,        ///< IEEE 1364-2005 keywords
    SystemVerilog2017,  ///< IEEE 1800-2017 (parsed, but RTL extraction support comes later)
};

[[nodiscard]] std::string_view toString(LanguageStd lang) noexcept;
[[nodiscard]] std::optional<LanguageStd> parseLanguageStd(std::string_view text) noexcept;

struct MacroDefine {
    std::string name;
    std::optional<std::string> value; ///< `define NAME VALUE; nullopt means `define NAME
};

struct FrontendInput {
    std::vector<std::filesystem::path> sourceFiles; ///< parsed in order as one compilation unit
    std::vector<std::filesystem::path> includeDirs;
    std::vector<MacroDefine> defines;
    std::string top; ///< empty: let the elaborator infer top-level modules
    LanguageStd language = LanguageStd::Verilog2001;
};

/// A module (or primitive/interface/program) definition found in the sources.
struct DefinitionInfo {
    std::string name;
    std::string kind; ///< "module", "interface", "program", ...
    SourceLocation location;
};

/// One node of the elaborated instance tree.
struct InstanceInfo {
    std::string name;       ///< instance name, e.g. "u_sync" or "u_sync[3]"
    std::string definition; ///< definition name, e.g. "sync_2ff"
    std::string hierPath;   ///< full hierarchical path, e.g. "top.g_bit[0].u_sync"
    SourceLocation location;
    std::vector<InstanceInfo> children;
};

/// What was found and how it elaborates (used for project/hierarchy views).
struct ElaborationSummary {
    std::vector<std::filesystem::path> parsedFiles; ///< sources plus resolved `include files
    std::vector<DefinitionInfo> definitions;        ///< sorted by name
    std::vector<InstanceInfo> topInstances;
    std::size_t instanceCount = 0;                  ///< all instances incl. tops
};

/// Everything the frontend produces.
struct FrontendOutput {
    ElaborationSummary summary;
    /// RTL-level Design IR. Null only if elaboration produced no usable design.
    std::shared_ptr<const ir::Design> design;
};

/// Abstract HDL frontend. Only the slang adapter implements it today; a VHDL
/// frontend will implement the same interface later.
class IFrontend {
public:
    IFrontend() = default;
    IFrontend(const IFrontend&) = delete;
    IFrontend& operator=(const IFrontend&) = delete;
    virtual ~IFrontend() = default;

    /// Parses, elaborates and extracts the Design IR. Every frontend
    /// diagnostic (syntax, semantic, IR extraction) is reported to `sink`.
    /// Returns an error only when nothing usable could be produced (e.g. a
    /// source file cannot be read); a design with errors still yields output
    /// so the user can see what was parsed. If the HDL has errors, the IR is
    /// not built (it would be based on an invalid AST) and `design` is null.
    [[nodiscard]] virtual Result<FrontendOutput> elaborate(const FrontendInput& input, DiagnosticSink& sink) = 0;
};

[[nodiscard]] std::unique_ptr<IFrontend> makeSlangFrontend();

} // namespace opencdc::frontend
