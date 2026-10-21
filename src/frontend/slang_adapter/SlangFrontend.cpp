// slang adapter: the ONLY translation unit family allowed to include slang headers.
// Everything that crosses this boundary is converted to OpenCDC types.

#include "frontend/Frontend.h"
#include "frontend/MixedSupport.h"

#include "core/PathUtil.h"
#include "frontend/slang_adapter/IrBuilder.h"

#include <slang/ast/Compilation.h>
#include <slang/ast/Symbol.h>
#include <slang/ast/symbols/BlockSymbols.h>
#include <slang/ast/symbols/CompilationUnitSymbols.h>
#include <slang/ast/symbols/InstanceSymbols.h>
#include <slang/diagnostics/DiagnosticEngine.h>
#include <slang/diagnostics/DeclarationsDiags.h>
#include <slang/diagnostics/Diagnostics.h>
#include <slang/parsing/LexerFacts.h>
#include <slang/parsing/Preprocessor.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/text/SourceManager.h>
#include <slang/util/Bag.h>
#include <slang/util/LanguageVersion.h>

#include <algorithm>
#include <exception>
#include <set>
#include <system_error>
#include <utility>

namespace opencdc::frontend {

std::string_view toString(LanguageStd lang) noexcept {
    switch (lang) {
        case LanguageStd::Verilog2001: return "verilog-2001";
        case LanguageStd::Verilog2005: return "verilog-2005";
        case LanguageStd::SystemVerilog2017: return "systemverilog-2017";
    }
    return "unknown";
}

std::optional<LanguageStd> parseLanguageStd(std::string_view text) noexcept {
    if (text == "verilog-2001")
        return LanguageStd::Verilog2001;
    if (text == "verilog-2005")
        return LanguageStd::Verilog2005;
    if (text == "systemverilog-2017")
        return LanguageStd::SystemVerilog2017;
    return std::nullopt;
}

namespace {

namespace sast = slang::ast;

class SlangFrontend final : public IFrontend {
public:
    Result<FrontendOutput> elaborate(const FrontendInput& input, DiagnosticSink& sink) override;
};

using detail::convertLocation;

Severity convertSeverity(slang::DiagnosticSeverity sev) {
    switch (sev) {
        case slang::DiagnosticSeverity::Ignored:
        case slang::DiagnosticSeverity::Note: return Severity::Info;
        case slang::DiagnosticSeverity::Warning: return Severity::Warning;
        case slang::DiagnosticSeverity::Error: return Severity::Error;
        case slang::DiagnosticSeverity::Fatal: return Severity::Fatal;
    }
    return Severity::Error;
}

/// Reports the diagnostics; returns whether any of them is an error.
bool reportSlangDiagnostics(const slang::Diagnostics& diags, const slang::SourceManager& sm, DiagnosticSink& sink,
                            const std::set<std::string>* lenientModules = nullptr) {
    slang::DiagnosticEngine engine(sm);
    (void)engine.setWarningOptions({}); // slang's default warning set
    bool errors = false;

    for (const auto& diag : diags) {
        auto sev = engine.getSeverity(diag.code, diag.location);
        if (sev == slang::DiagnosticSeverity::Ignored)
            continue;

        Diagnostic out;
        out.code = "FE-SLANG-" + std::string(slang::toString(diag.code));
        out.severity = convertSeverity(sev);
        out.message = engine.formatMessage(diag);
        out.location = convertLocation(sm, diag.location);
        if (lenientModules && diag.code == slang::diag::PortDoesNotExist) {
            // port 'X' does not exist in 'FDRE'
            const auto q = out.message.rfind(" in '");
            if (q != std::string::npos && out.message.size() > q + 6) {
                const std::string module = out.message.substr(q + 5, out.message.size() - q - 6);
                if (lenientModules->contains(module)) {
                    const auto p0 = out.message.find('\'');
                    const auto p1 = out.message.find('\'', p0 + 1);
                    out.code = "FE-PRIMITIVE-PORT";
                    out.severity = Severity::Info;
                    out.message = "port " + out.message.substr(p0, p1 - p0 + 1) + " of vendor primitive '" + module +
                                  "' is not modelled; its connection is ignored";
                }
            }
        }
        errors = errors || out.isError();
        for (const auto& note : diag.notes) {
            std::string text = engine.formatMessage(note);
            if (auto loc = toString(convertLocation(sm, note.location)); !loc.empty())
                text = loc + ": " + text;
            out.notes.push_back(std::move(text));
        }
        sink.report(std::move(out));
    }
    return errors;
}

class InstanceCollector {
public:
    explicit InstanceCollector(const slang::SourceManager& sm) : sm_(sm) {}

    std::size_t count() const noexcept { return count_; }

    InstanceInfo makeInstance(const sast::InstanceSymbol& inst, std::string_view parentPath) {
        InstanceInfo info;
        info.hierPath = inst.getHierarchicalPath();
        // Display name relative to the parent instance, so generate scopes
        // stay visible: "g_bit[0].u_sync".
        if (!parentPath.empty() && info.hierPath.size() > parentPath.size() + 1 &&
            info.hierPath.starts_with(parentPath)) {
            info.name = info.hierPath.substr(parentPath.size() + 1);
        }
        else {
            info.name = info.hierPath;
        }
        info.definition = std::string(inst.getDefinition().name);
        info.location = convertLocation(sm_, inst.location);
        ++count_;
        collectScope(inst.body, info.hierPath, info.children);
        return info;
    }

private:
    void collectScope(const sast::Scope& scope, std::string_view parentPath,
                      std::vector<InstanceInfo>& out) {
        for (const auto& member : scope.members())
            collectSymbol(member, parentPath, out);
    }

    void collectSymbol(const sast::Symbol& sym, std::string_view parentPath,
                       std::vector<InstanceInfo>& out) {
        switch (sym.kind) {
            case sast::SymbolKind::Instance:
                out.push_back(makeInstance(sym.as<sast::InstanceSymbol>(), parentPath));
                break;
            case sast::SymbolKind::InstanceArray:
                for (const auto* elem : sym.as<sast::InstanceArraySymbol>().elements)
                    collectSymbol(*elem, parentPath, out);
                break;
            case sast::SymbolKind::GenerateBlock: {
                const auto& block = sym.as<sast::GenerateBlockSymbol>();
                if (!block.isUninstantiated)
                    collectScope(block, parentPath, out);
                break;
            }
            case sast::SymbolKind::GenerateBlockArray:
                for (const auto* entry : sym.as<sast::GenerateBlockArraySymbol>().entries)
                    collectSymbol(*entry, parentPath, out);
                break;
            default:
                break;
        }
    }

    const slang::SourceManager& sm_;
    std::size_t count_ = 0;
};

Result<FrontendOutput> SlangFrontend::elaborate(const FrontendInput& input, DiagnosticSink& sink) {
    return detail::elaborateVerilog(input, sink, nullptr);
}

} // namespace

Result<FrontendOutput> detail::elaborateVerilog(const FrontendInput& input, DiagnosticSink& sink,
                                                const VerilogMixedOptions* mixed) {
    if (input.sourceFiles.empty()) {
        return Diagnostic{"FE-NO-SOURCES", Severity::Error, "no HDL source files were given", {}, {}};
    }

    // Check files up front so a missing file gets a precise diagnostic.
    std::vector<std::string> paths;
    std::vector<std::string> keywordPatterns;
    for (const auto& file : input.sourceFiles) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec)) {
            return Diagnostic{"FE-FILE-NOT-FOUND", Severity::Error,
                              "source file not found: " + toUtf8(file), {}, {}};
        }
        auto canonical = std::filesystem::weakly_canonical(std::filesystem::absolute(file, ec), ec);
        paths.push_back(toUtf8(canonical));
        // slang matches keyword mappings against its own weakly-canonical,
        // native-format path of each buffer, so use the same form here.
        // On Windows, path::string() uses the ANSI code page (as slang does
        // when it converts the pattern back) and throws if a character is
        // not representable; fall back to the default keyword set then.
        try {
            keywordPatterns.push_back(canonical.string());
        }
        catch (const std::exception&) {
            sink.report({"FE-KEYWORD-MAPPING", Severity::Warning,
                         "cannot apply Verilog keyword restrictions to '" + toUtf8(canonical) +
                             "' (path not representable in the system code page); "
                             "1364-2005 keywords are used for this file",
                         {}, {}});
        }
    }
    for (const auto& dir : input.includeDirs) {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) {
            sink.report({"FE-INCDIR-NOT-FOUND", Severity::Warning,
                         "include directory not found: " + toUtf8(dir), {}, {}});
        }
    }

    slang::parsing::PreprocessorOptions ppOptions;
    slang::ast::CompilationOptions compOptions;
    // slang's default (131072) guards against runaway generate loops; large
    // flat designs (a 300k-iteration generate) legitimately need more.
    compOptions.maxGenerateSteps = 1U << 24;
    for (const auto& def : input.defines)
        ppOptions.predefines.push_back(def.value ? def.name + "=" + *def.value : def.name);
    for (const auto& dir : input.includeDirs)
        ppOptions.additionalIncludePaths.push_back(dir);

    switch (input.language) {
        case LanguageStd::Verilog2001:
        case LanguageStd::Verilog2005: {
            ppOptions.languageVersion = slang::LanguageVersion::v1364_2005;
            compOptions.languageVersion = slang::LanguageVersion::v1364_2005;
            // Restrict keywords per file so that e.g. a Verilog net named
            // `logic` or `bit` is not mistaken for a SystemVerilog keyword.
            auto kw = input.language == LanguageStd::Verilog2001
                          ? slang::parsing::KeywordVersion::v1364_2001
                          : slang::parsing::KeywordVersion::v1364_2005;
            for (const auto& p : keywordPatterns)
                ppOptions.keywordMapping.emplace_back(p, kw);
            break;
        }
        case LanguageStd::SystemVerilog2017:
            ppOptions.languageVersion = slang::LanguageVersion::v1800_2017;
            compOptions.languageVersion = slang::LanguageVersion::v1800_2017;
            break;
    }
    if (mixed) {
        compOptions.flags |= sast::CompilationFlags::IgnoreUnknownModules;
        for (const auto& t : mixed->tops)
            compOptions.topModules.emplace(t);
    }
    else if (!input.top.empty()) {
        compOptions.topModules.emplace(input.top);
    }
    for (const auto& [name, value] : input.topParameters)
        compOptions.paramOverrides.push_back(name + "=" + value);

    slang::Bag options;
    options.set(ppOptions);
    options.set(compOptions);

    slang::SourceManager sourceManager;
    std::vector<std::string_view> pathViews(paths.begin(), paths.end());
    // All files form one compilation unit (macros flow from earlier files to
    // later ones), matching the behaviour of FPGA vendor tools.
    auto treeOrError = slang::syntax::SyntaxTree::fromFiles(pathViews, sourceManager, options);
    if (!treeOrError) {
        const auto& [ec, path] = treeOrError.error();
        return Diagnostic{"FE-FILE-READ", Severity::Error,
                          "cannot read '" + std::string(path) + "': " + ec.message(), {}, {}};
    }

    sast::Compilation compilation(options);
    compilation.addSyntaxTree(*treeOrError);
    const auto& root = compilation.getRoot();

    const auto& slangDiags = compilation.getAllDiagnostics();
    const bool hdlErrors =
        reportSlangDiagnostics(slangDiags, sourceManager, sink, mixed ? &mixed->lenientModules : nullptr);

    FrontendOutput output;
    ElaborationSummary& summary = output.summary;

    std::set<std::string> files;
    for (auto buffer : sourceManager.getAllBuffers()) {
        const auto& full = sourceManager.getFullPath(buffer);
        if (!full.empty())
            files.insert(toUtf8(full));
    }
    summary.parsedFiles.assign(files.begin(), files.end());

    for (const auto* sym : compilation.getDefinitions()) {
        if (sym->kind != sast::SymbolKind::Definition)
            continue; // primitives etc. are not reported in stage 1
        const auto& def = sym->as<sast::DefinitionSymbol>();
        summary.definitions.push_back(
            {std::string(def.name), std::string(def.getKindString()), convertLocation(sourceManager, def.location)});
        if (mixed && mixed->moduleParams) {
            auto& names = (*mixed->moduleParams)[std::string(def.name)];
            for (const auto& p : def.parameters)
                if (!p.isLocalParam && !p.isTypeParam)
                    names.emplace_back(p.name);
        }
    }
    std::ranges::sort(summary.definitions, {}, &DefinitionInfo::name);

    InstanceCollector collector(sourceManager);
    for (const auto* top : root.topInstances)
        summary.topInstances.push_back(collector.makeInstance(*top, {}));
    summary.instanceCount = collector.count();

    if (hdlErrors) {
        sink.report({"IR-SKIPPED", Severity::Info,
                     "RTL extraction skipped because the HDL sources contain errors", {}, {}});
    }
    else {
        detail::IrBuilder builder(sourceManager, sink);
        if (mixed)
            builder.setForeignDefinitions(mixed->foreignModules, true);
        output.design = builder.build(root);
    }
    return output;
}

std::unique_ptr<IFrontend> makeSlangFrontend() {
    return std::make_unique<SlangFrontend>();
}

} // namespace opencdc::frontend
