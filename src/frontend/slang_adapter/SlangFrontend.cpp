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
#include <slang/diagnostics/ExpressionsDiags.h>
#include <slang/diagnostics/LexerDiags.h>
#include <slang/parsing/LexerFacts.h>
#include <slang/parsing/Preprocessor.h>
#include <slang/syntax/SyntaxTree.h>
#include <slang/text/SourceManager.h>
#include <slang/util/String.h>
#include <regex>
#include "core/TextEncoding.h"
#include <slang/util/Bag.h>
#include <slang/util/LanguageVersion.h>

#include <algorithm>
#include <cctype>
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
/// A source range of a module the design never instantiates.
struct UnusedRange {
    slang::BufferID buffer;
    std::size_t begin = 0, end = 0;
    std::string module;
};

bool reportSlangDiagnostics(const slang::Diagnostics& diags, const slang::SourceManager& sm, DiagnosticSink& sink,
                            const std::set<std::string>* lenientModules = nullptr,
                            const std::vector<UnusedRange>* unused = nullptr,
                            const slang::Diagnostics* parseDiags = nullptr) {
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
        // Vivado's simulation netlists write cell paths with '\' into attribute strings
        // ("\AXI_LITE_REG_INTERFACE_I/..."): not a problem of the design
        if (diag.code == slang::diag::UnknownEscapeCode && out.location.file.ends_with("_sim_netlist.v"))
            continue;
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
        if (diag.code == slang::diag::UnknownModule)
            out.notes.emplace_back(detail::kUnknownModuleHint);
        // an output port connected to a constant (.M00_AXI_ARQOS(4'b0), Loongson chiplab):
        // an error in SystemVerilog, Vivado ignores the connection
        if (diag.code == slang::diag::ExpressionNotAssignable && out.isError() && diag.location.valid()) {
            const auto loc = sm.getFullyOriginalLoc(diag.location);
            const std::string_view text = sm.getSourceText(loc.buffer());
            if (loc.offset() <= text.size()) {
                std::size_t i = loc.offset();
                auto skipSpace = [&] {
                    while (i > 0 && std::isspace(static_cast<unsigned char>(text[i - 1])))
                        --i;
                };
                skipSpace();
                bool port = i > 0 && text[i - 1] == '(';
                if (port) {
                    --i;
                    skipSpace();
                    const std::size_t end = i;
                    while (i > 0 && (std::isalnum(static_cast<unsigned char>(text[i - 1])) || text[i - 1] == '_' ||
                                     text[i - 1] == '$'))
                        --i;
                    port = i < end;
                    skipSpace();
                    port = port && i > 0 && text[i - 1] == '.';
                }
                if (port) {
                    out.code = "FE-OUTPUT-PORT-CONSTANT";
                    out.severity = Severity::Warning;
                    out.message = "output port connected to an expression that cannot be assigned (a constant); the "
                                  "connection is ignored, as Vivado does";
                }
            }
        }
        // a semantic error inside a module the design never instantiates: Vivado and
        // Quartus elaborate from the top and do not look at it; syntax errors stay errors
        if (unused && out.isError() && diag.location.valid()) {
            const auto sub = diag.code.getSubsystem();
            bool syntax = sub == slang::DiagSubsystem::Lexer || sub == slang::DiagSubsystem::Preprocessor ||
                          sub == slang::DiagSubsystem::Parser;
            if (parseDiags)
                for (const auto& pd : *parseDiags)
                    if (pd.code == diag.code && pd.location == diag.location) {
                        syntax = true;
                        break;
                    }
            const auto loc = sm.getFullyOriginalLoc(diag.location);
            for (const auto& u : *unused)
                if (!syntax && loc.buffer() == u.buffer && loc.offset() >= u.begin && loc.offset() < u.end) {
                    out.severity = Severity::Warning;
                    out.notes.push_back("in module '" + u.module + "', which the design does not instantiate (FPGA "
                                        "tools elaborate from the top and do not check it)");
                    break;
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

/// Source files and the files they `include that are not UTF-8 (GBK comments
/// from Chinese Windows editors, Latin-1, UTF-16) are decoded and handed to
/// slang as UTF-8 under the path slang caches files by, so slang reads the
/// decoded text instead of warning about every comment.
void preloadNonUtf8(const std::vector<std::string>& paths, const std::vector<std::filesystem::path>& includeDirs,
                    slang::SourceManager& sm, DiagnosticSink& sink,
                    std::map<std::string, std::string>* includedBy = nullptr) {
    static const std::regex includeRe(R"re(`include\s*"([^"]+)")re");
    std::set<std::string> visited;
    std::vector<std::filesystem::path> work;
    for (const auto& p : paths)
        work.push_back(fromUtf8(p));
    while (!work.empty()) {
        const std::filesystem::path file = work.back();
        work.pop_back();
        std::error_code ec;
        const auto canonical = std::filesystem::weakly_canonical(file, ec);
        const std::string key = slang::getU8Str(ec ? file : canonical);
        if (!visited.insert(key).second)
            continue;
        auto text = readSourceText(file);
        if (!text)
            continue;
        if (text->transcoded()) {
            sm.assignText(key, text->utf8);
            sink.report({"FE-ENCODING", Severity::Info,
                         "file is not UTF-8: read as " + text->encoding + " (and converted to UTF-8)",
                         {key, 0, 0}, {}});
        }
        for (std::sregex_iterator it(text->utf8.begin(), text->utf8.end(), includeRe), end; it != end; ++it) {
            const std::filesystem::path inc = fromUtf8((*it)[1].str());
            std::vector<std::filesystem::path> candidates{file.parent_path() / inc};
            for (const auto& d : includeDirs)
                candidates.push_back(d / inc);
            for (const auto& c : candidates)
                if (std::filesystem::is_regular_file(c, ec)) {
                    if (includedBy) {
                        const auto cc = std::filesystem::weakly_canonical(c, ec);
                        includedBy->emplace(slang::getU8Str(ec ? c : cc), key);
                    }
                    work.push_back(c);
                    break;
                }
        }
    }
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
    // Design elements without `timescale next to ones with it are accepted
    // (as by FPGA tools): they get 1ns/1ps. The generated mixed-language
    // wrapper relies on this as well.
    compOptions.defaultTimeScale = slang::TimeScale(
        slang::TimeScaleValue(slang::TimeUnit::Nanoseconds, slang::TimeScaleMagnitude::One),
        slang::TimeScaleValue(slang::TimeUnit::Picoseconds, slang::TimeScaleMagnitude::One));
    // Vivado and Quartus accept a net used before its declaration (implicit
    // forward reference, common in vendor IP such as ADI's axi_ad9361)
    compOptions.flags |= sast::CompilationFlags::AllowUseBeforeDeclare;
    for (const auto& def : input.defines)
        ppOptions.predefines.push_back(def.value ? def.name + "=" + *def.value : def.name);
    for (const auto& dir : input.includeDirs)
        ppOptions.additionalIncludePaths.push_back(dir);

    bool svKeywordMapping = false;
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
            // as in Vivado (file type Verilog for .v): a .v file of a SystemVerilog
            // project keeps the Verilog-2005 keywords, so a port named `int` or
            // `logic` there is an identifier (the Loongson NSCSCC mycpu_top.v)
            // (every source and every file it includes is mapped below, after the
            // includes are known: slang's keyword stack stays balanced only for
            // mapped buffers, an unmapped include inside an unmapped include pops
            // the includer's entry)
            svKeywordMapping = true;
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

    slang::SourceManager sourceManager;
    std::map<std::string, std::string> includedBy;
    preloadNonUtf8(paths, input.includeDirs, sourceManager, sink, svKeywordMapping ? &includedBy : nullptr);
    if (svKeywordMapping) {
        std::map<std::string, slang::parsing::KeywordVersion> version;
        for (const auto& p : keywordPatterns)
            version[p] = p.ends_with(".v") || p.ends_with(".V") ? slang::parsing::KeywordVersion::v1364_2005
                                                                 : slang::parsing::KeywordVersion::v1800_2017;
        // an include takes the keywords of the file that first includes it
        std::function<slang::parsing::KeywordVersion(const std::string&, int)> of =
            [&](const std::string& f, int depth) -> slang::parsing::KeywordVersion {
            if (auto it = version.find(f); it != version.end())
                return it->second;
            auto by = includedBy.find(f);
            if (by == includedBy.end() || depth > 64)
                return slang::parsing::KeywordVersion::v1800_2017;
            return version[f] = of(by->second, depth + 1);
        };
        for (const auto& [inc, by] : includedBy)
            (void)of(inc, 0);
        for (const auto& [f, v] : version)
            ppOptions.keywordMapping.emplace_back(f, v);
    }

    slang::Bag options;
    options.set(ppOptions);
    options.set(compOptions);
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
    std::vector<UnusedRange> unused;
    for (const auto* def : compilation.getUnreferencedDefinitions()) {
        const auto* syn = def->getSyntax();
        if (!syn)
            continue;
        const auto r = syn->sourceRange();
        const auto b = sourceManager.getFullyOriginalLoc(r.start());
        const auto e = sourceManager.getFullyOriginalLoc(r.end());
        if (b.buffer() == e.buffer())
            unused.push_back({b.buffer(), b.offset(), e.offset(), std::string(def->name)});
    }
    const bool hdlErrors = reportSlangDiagnostics(slangDiags, sourceManager, sink,
                                                  mixed ? &mixed->lenientModules : nullptr,
                                                  input.top.empty() && !mixed ? nullptr : &unused,
                                                  &compilation.getParseDiagnostics());

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
        if (mixed) {
            builder.setForeignDefinitions(mixed->foreignModules, true);
            builder.setBlackBoxUnknown(mixed->blackBoxUnknown);
        }
        output.design = builder.build(root);
    }
    return output;
}

std::unique_ptr<IFrontend> makeSlangFrontend() {
    return std::make_unique<SlangFrontend>();
}

} // namespace opencdc::frontend
