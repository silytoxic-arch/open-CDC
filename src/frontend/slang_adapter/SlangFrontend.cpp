// slang adapter: the ONLY translation unit family allowed to include slang headers.
// Everything that crosses this boundary is converted to OpenCDC types.

#include "frontend/Frontend.h"
#include "frontend/MixedSupport.h"

#include "core/PathUtil.h"
#include "frontend/slang_adapter/IrBuilder.h"

#include <slang/ast/ASTVisitor.h>
#include <slang/ast/Compilation.h>
#include <slang/ast/EvalContext.h>
#include <slang/ast/Symbol.h>
#include <slang/ast/symbols/BlockSymbols.h>
#include <slang/ast/symbols/CompilationUnitSymbols.h>
#include <slang/ast/symbols/InstanceSymbols.h>
#include <slang/diagnostics/DiagnosticEngine.h>
#include <slang/diagnostics/DeclarationsDiags.h>
#include <slang/diagnostics/Diagnostics.h>
#include <slang/diagnostics/ExpressionsDiags.h>
#include <slang/diagnostics/LexerDiags.h>
#include <slang/diagnostics/LookupDiags.h>
#include <slang/diagnostics/ParserDiags.h>
#include <slang/parsing/Lexer.h>
#include <slang/parsing/LexerFacts.h>
#include <slang/parsing/Preprocessor.h>
#include <slang/syntax/AllSyntax.h>
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
#include <functional>
#include <memory>
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

/// Branches of procedural 'if' statements whose condition is constant in every
/// instance: never taken (dead) and taken (live) source ranges.
struct ConstantBranches {
    std::vector<UnusedRange> dead, live;
    [[nodiscard]] bool onlyDead(slang::SourceLocation loc) const {
        auto in = [&](const std::vector<UnusedRange>& v) {
            return std::ranges::any_of(v, [&](const UnusedRange& r) {
                return loc.buffer() == r.buffer && loc.offset() >= r.begin && loc.offset() < r.end;
            });
        };
        if (!in(dead))
            return false;
        // the innermost range decides: a live branch nested in a dead one stays dead
        const UnusedRange* best = nullptr;
        bool bestDead = false;
        for (const auto* v : {&dead, &live})
            for (const auto& r : *v)
                if (loc.buffer() == r.buffer && loc.offset() >= r.begin && loc.offset() < r.end &&
                    (!best || r.end - r.begin < best->end - best->begin ||
                     (r.end - r.begin == best->end - best->begin && v == &live))) {
                    best = &r;
                    bestDead = v == &dead;
                }
        return bestDead;
    }
};

/// Collects the branches of procedural 'if (constant)' statements in the elaborated
/// design (corundum pcie_tlp_fifo_raw.v: `if (OUT_TLP_SEG_COUNT > 1) ... x[i +: SEG_SEL_WIDTH]`
/// with SEG_SEL_WIDTH = 0 when the count is 1). The same statement in another
/// instance, with other parameters, may take the other branch: it is then live.
struct ConstantBranchCollector
    : sast::ASTVisitor<ConstantBranchCollector, sast::VisitFlags::Statements | sast::VisitFlags::Expressions |
                                                    sast::VisitFlags::Bad> { // the erroneous code is in "bad" nodes
    const slang::SourceManager& sm;
    ConstantBranches& out;
    const sast::InstanceBodySymbol* body = nullptr;
    ConstantBranchCollector(const slang::SourceManager& m, ConstantBranches& o) : sm(m), out(o) {}
    /// The range of a statement as offsets in its file (none when slang kept no range:
    /// a statement containing an error, and the block around it).
    std::optional<UnusedRange> rangeOf(slang::SourceRange r) const {
        if (!r.start().valid() || !r.end().valid())
            return std::nullopt;
        const auto b = sm.getFullyOriginalLoc(r.start());
        const auto e = sm.getFullyOriginalLoc(r.end());
        if (b.buffer() != e.buffer())
            return std::nullopt;
        return UnusedRange{b.buffer(), b.offset(), e.offset(), {}};
    }
    void handle(const sast::InstanceBodySymbol& b) {
        const auto* saved = std::exchange(body, &b);
        visitDefault(b);
        body = saved;
    }
    // slang wraps a statement containing an error in an InvalidStatement
    void handle(const sast::InvalidStatement& s) {
        if (s.child)
            s.child->visit(*this);
    }
    void handle(const sast::ConditionalStatement& c) {
        if (body && c.conditions.size() == 1 && !c.conditions[0].pattern) {
            sast::EvalContext ctx(*body);
            const auto v = c.conditions[0].expr->eval(ctx);
            const auto whole = rangeOf(c.sourceRange);
            const auto cond = rangeOf(c.conditions[0].expr->sourceRange);
            if (v.isInteger() && !v.integer().hasUnknown() && whole && cond && cond->buffer == whole->buffer) {
                // a branch without a range of its own spans the gap up to its neighbour
                auto then = rangeOf(c.ifTrue.sourceRange);
                auto other = c.ifFalse ? rangeOf(c.ifFalse->sourceRange) : std::nullopt;
                if (!then)
                    then = UnusedRange{whole->buffer, cond->end, other ? other->begin : whole->end, {}};
                if (c.ifFalse && !other && then->end <= whole->end)
                    other = UnusedRange{whole->buffer, then->end, whole->end, {}};
                const bool taken = static_cast<bool>(v.integer() != 0);
                (taken ? out.live : out.dead).push_back(*then);
                if (other)
                    (taken ? out.dead : out.live).push_back(*other);
            }
        }
        visitDefault(c);
    }
};

bool reportSlangDiagnostics(const slang::Diagnostics& diags, const slang::SourceManager& sm, DiagnosticSink& sink,
                            const std::set<std::string>* lenientModules = nullptr,
                            const std::vector<UnusedRange>* unused = nullptr,
                            const slang::Diagnostics* parseDiags = nullptr,
                            const ConstantBranches* branches = nullptr) {
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
        // a Verilog file (.v) compiled as SystemVerilog: its procedural declarations with
        // initializers are static, as Verilog-2001 defines them (Quartus and Vivado read them so)
        if (diag.code == slang::diag::StaticInitializerMustBeExplicit) {
            if (out.location.file.ends_with(".v") || out.location.file.ends_with(".vh") ||
                out.location.file.ends_with(".V"))
                continue;
            // in SystemVerilog the 'static' keyword is required; Quartus and Vivado
            // accept the declaration without it and read it as OpenCDC does: the
            // initializer is the power-up value, applied once
            out.code = "FE-IMPLICIT-STATIC";
            out.severity = Severity::Info;
            out.message = "static variable with an initializer declared in a procedural block without 'static': "
                          "the initializer is its power-up value, applied once (as synthesis tools read it)";
        }
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
            // a semantic error in a branch of 'if (constant)' that no instance takes:
            // synthesis does not elaborate it (and OpenCDC does not lower it)
            if (!syntax && out.isError() && branches && branches->onlyDead(loc)) {
                out.severity = Severity::Warning;
                out.notes.push_back("in a branch of an 'if' whose condition is constant and false in every instance "
                                    "(FPGA tools do not elaborate that branch)");
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
                    std::map<std::string, std::string>* includedBy = nullptr,
                    const std::map<std::string, std::string>* overrides = nullptr) {
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
        if (overrides) {
            if (const auto it = overrides->find(key); it != overrides->end()) {
                // the text with its corrections (below: nets assigned procedurally)
                text->utf8 = it->second;
                sm.assignText(key, text->utf8);
            }
            else if (text->transcoded())
                sm.assignText(key, text->utf8);
        }
        else if (text->transcoded())
            sm.assignText(key, text->utf8);
        if (text->transcoded() && !(overrides && overrides->contains(key))) {
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

/// Corrections of constructs that are errors in (System)Verilog but that Quartus
/// accepts, so that designs built with Quartus can be analyzed (MiSTer's
/// framework relies on both):
/// - a net assigned in a procedural block is read as a variable: its `wire`
///   keyword becomes `reg ` (same length), a port without a type gets `reg`;
/// - a name declared in a generate block and used outside it in the enclosing
///   module (an undeclared identifier): its declaration is moved before the
///   generate construct (on the same line);
/// - a function port declared `wire`: the keyword is removed (an input).
/// The edits are applied to the source text and the design compiled again.
struct SourceEdit {
    std::string file; ///< the buffer's canonical path (as preloadNonUtf8 keys it)
    std::size_t offset = 0;
    std::size_t remove = 0;
    std::string insert;
    std::string code, message, note;
    SourceLocation where;
};

std::string bufferKey(const slang::SourceManager& sm, slang::BufferID b) {
    std::error_code ec;
    const std::filesystem::path full = sm.getFullPath(b);
    const auto canonical = std::filesystem::weakly_canonical(full, ec);
    return slang::getU8Str(ec ? full : canonical);
}

/// The identifier at a location.
std::string identifierAt(const slang::SourceManager& sm, slang::SourceLocation loc) {
    const auto text = sm.getSourceText(loc.buffer());
    std::size_t e = loc.offset();
    while (e < text.size() && (std::isalnum(static_cast<unsigned char>(text[e])) || text[e] == '_' || text[e] == '$'))
        ++e;
    return std::string(text.substr(loc.offset(), e - loc.offset()));
}

/// Whether the text before a location (skipping whitespace) ends in '(' or ',': the start of an item in a port list.
bool afterListSeparator(const slang::SourceManager& sm, slang::SourceLocation loc) {
    const auto text = sm.getSourceText(loc.buffer());
    std::size_t i = loc.offset();
    while (i > 0 && std::isspace(static_cast<unsigned char>(text[i - 1])))
        --i;
    return i > 0 && (text[i - 1] == '(' || text[i - 1] == ',');
}

std::vector<SourceEdit> quartusCompatEdits(const slang::Diagnostics& diags, const slang::syntax::SyntaxTree& tree,
                                           const slang::SourceManager& sm, const sast::RootSymbol& root) {
    using namespace slang::syntax;
    std::vector<SourceEdit> edits;
    // ---- nets assigned in procedural blocks ----
    std::map<std::uint32_t, std::vector<std::pair<std::size_t, std::string>>> netUses;
    std::vector<std::pair<slang::SourceLocation, std::string>> undeclared;
    for (const auto& d : diags) {
        const auto loc = sm.getFullyOriginalLoc(d.location);
        if (d.code == slang::diag::AssignToNet) {
            if (auto id = identifierAt(sm, loc); !id.empty())
                netUses[loc.buffer().getId()].emplace_back(loc.offset(), std::move(id));
        }
        else if (d.code == slang::diag::UndeclaredIdentifier || d.code == slang::diag::TypoIdentifier) {
            if (auto id = identifierAt(sm, loc); !id.empty())
                undeclared.emplace_back(loc, std::move(id));
        }
        else if ((d.code == slang::diag::ExpectedFunctionPort || d.code == slang::diag::WireDataType) &&
                 identifierAt(sm, loc) == "wire" && afterListSeparator(sm, loc)) {
            // `function [9:0] f(wire signed [10:0] x)` (C64_MiSTer sid_tables.sv): a function
            // port is a variable; Quartus reads `wire` as an input of that type. The keyword
            // becomes spaces (an implicitly typed input)
            const auto key = bufferKey(sm, loc.buffer());
            if (std::ranges::none_of(edits, [&](const SourceEdit& e) { return e.file == key && e.offset == loc.offset(); }))
                edits.push_back({key, loc.offset(), 4, "    ", "FE-WIRE-FUNCTION-PORT",
                                 "function port declared as 'wire', which (System)Verilog does not allow: read as "
                                 "an input of that type, as Quartus does",
                                 "remove 'wire' (or write 'input') to make the design portable",
                                 convertLocation(sm, loc)});
        }
    }
    if (!netUses.empty()) {
        struct Decl {
            std::uint32_t buffer;
            std::size_t declOffset;
            std::string name;
            slang::SourceLocation loc;
            std::size_t editOffset, remove;
            std::string insert;
            bool inout = false;
        };
        std::vector<Decl> decls;
        const auto text = [&](slang::BufferID b) { return sm.getSourceText(b); };
        // `wire` -> `reg `
        const auto addWire = [&](slang::parsing::Token netType, const DeclaratorSyntax& d) {
            if (netType.kind != slang::parsing::TokenKind::WireKeyword || d.initializer)
                return;
            const auto k = sm.getFullyOriginalLoc(netType.location());
            const auto n = sm.getFullyOriginalLoc(d.name.location());
            if (k.buffer() == n.buffer())
                decls.push_back({n.buffer().getId(), n.offset(), std::string(d.name.valueText()), n, k.offset(), 4,
                                 "reg "});
        };
        // `output [7:0] x` (a net) -> `output reg [7:0] x`; `inout reg x` -> `output reg x`
        const auto addImplicit = [&](const VariablePortHeaderSyntax& h, const DeclaratorSyntax& d) {
            if (h.direction.kind == slang::parsing::TokenKind::InOutKeyword) {
                const auto k = sm.getFullyOriginalLoc(h.direction.location());
                const auto n = sm.getFullyOriginalLoc(d.name.location());
                if (k.buffer() != n.buffer())
                    return;
                const auto t = text(k.buffer());
                const std::size_t after = k.offset() + 5;
                const bool twoSpaces = after + 1 < t.size() && (t[after] == ' ' || t[after] == '\t') &&
                                       (t[after + 1] == ' ' || t[after + 1] == '\t');
                decls.push_back({n.buffer().getId(), n.offset(), std::string(d.name.valueText()), n, k.offset(),
                                 twoSpaces ? 6u : 5u, "output", true});
                return;
            }
            if (h.varKeyword || h.dataType->kind != SyntaxKind::ImplicitType || !h.direction)
                return;
            const auto k = sm.getFullyOriginalLoc(h.direction.location());
            const auto n = sm.getFullyOriginalLoc(d.name.location());
            if (k.buffer() != n.buffer())
                return;
            const auto t = text(k.buffer());
            const std::size_t after = k.offset() + h.direction.rawText().size();
            std::size_t ws = 0;
            while (after + ws < t.size() && (t[after + ws] == ' ' || t[after + ws] == '\t'))
                ++ws;
            if (ws >= 5) // "output     x" -> "output reg  x": the same length
                decls.push_back({n.buffer().getId(), n.offset(), std::string(d.name.valueText()), n, after + 1, 4,
                                 "reg "});
            else
                decls.push_back({n.buffer().getId(), n.offset(), std::string(d.name.valueText()), n, after, 0,
                                 " reg"});
        };
        std::function<void(const SyntaxNode&)> walk = [&](const SyntaxNode& node) {
            if (node.kind == SyntaxKind::NetDeclaration) {
                const auto& nd = node.as<NetDeclarationSyntax>();
                if (!nd.strength && !nd.delay)
                    for (const auto* d : nd.declarators)
                        addWire(nd.netType, *d);
            }
            else if (node.kind == SyntaxKind::ImplicitAnsiPort) {
                const auto& ip = node.as<ImplicitAnsiPortSyntax>();
                if (ip.header->kind == SyntaxKind::NetPortHeader)
                    addWire(ip.header->as<NetPortHeaderSyntax>().netType, *ip.declarator);
                else if (ip.header->kind == SyntaxKind::VariablePortHeader)
                    addImplicit(ip.header->as<VariablePortHeaderSyntax>(), *ip.declarator);
            }
            else if (node.kind == SyntaxKind::PortDeclaration) {
                const auto& pd = node.as<PortDeclarationSyntax>();
                if (pd.header->kind == SyntaxKind::NetPortHeader)
                    for (const auto* d : pd.declarators)
                        addWire(pd.header->as<NetPortHeaderSyntax>().netType, *d);
            }
            for (std::size_t i = 0; i < node.getChildCount(); ++i)
                if (const auto* c = node.childNode(i))
                    walk(*c);
        };
        walk(tree.root());
        std::set<std::pair<std::uint32_t, std::size_t>> done;
        for (const auto& [buffer, list] : netUses)
            for (const auto& [offset, name] : list) {
                // the nearest declaration of the name before the use, else after it (a use before
                // the declaration is accepted, as by Quartus and Vivado)
                const Decl* best = nullptr;
                for (const auto& d : decls)
                    if (d.buffer == buffer && d.name == name && d.declOffset < offset &&
                        (!best || d.declOffset > best->declOffset))
                        best = &d;
                if (!best)
                    for (const auto& d : decls)
                        if (d.buffer == buffer && d.name == name && d.declOffset > offset &&
                            (!best || d.declOffset < best->declOffset))
                            best = &d;
                if (!best) {
                    // declared in a file included by the use's file (ports in an included header)
                    for (const auto& d : decls)
                        if (d.name == name && sm.getIncludedFrom(d.loc.buffer()).buffer().getId() == buffer) {
                            best = &d;
                            break;
                        }
                }
                if (!best || !done.emplace(buffer, best->editOffset).second)
                    continue;
                if (best->inout)
                    edits.push_back({bufferKey(sm, best->loc.buffer()), best->editOffset, best->remove,
                                     best->insert, "FE-NET-PROCEDURAL-ASSIGN",
                                     "inout port '" + name + "' is assigned in a procedural block, which "
                                     "(System)Verilog does not allow: read as an output driven by the module, as "
                                     "Quartus drives it (its value from outside is not analyzed)",
                                     "drive the port with a continuous assignment from a variable to make the "
                                     "design portable",
                                     convertLocation(sm, best->loc)});
                else
                    edits.push_back({bufferKey(sm, best->loc.buffer()), best->editOffset, best->remove,
                                     best->insert, "FE-NET-PROCEDURAL-ASSIGN",
                                     "net '" + name + "' is assigned in a procedural block, which (System)Verilog "
                                                      "does not allow: read as a variable, as Quartus does",
                                     "declare it as a variable (logic or reg) to make the design portable",
                                     convertLocation(sm, best->loc)});
            }
    }
    // ---- names of generate blocks used in the enclosing module: their declarations are moved out ----
    if (!undeclared.empty()) {
        // the module bodies (one per definition and parameterization is enough: syntax is shared)
        std::vector<const sast::InstanceBodySymbol*> bodies;
        std::set<const sast::InstanceBodySymbol*> seenBodies;
        std::function<void(const sast::Symbol&, int)> visit = [&](const sast::Symbol& sym, int depth) {
            if (depth > 256)
                return;
            if (sym.kind == sast::SymbolKind::Instance) {
                const auto& body = sym.as<sast::InstanceSymbol>().body;
                if (seenBodies.insert(&body).second)
                    bodies.push_back(&body);
                for (const auto& m : body.members())
                    visit(m, depth + 1);
            }
            else if (sym.kind == sast::SymbolKind::GenerateBlock) {
                const auto& g = sym.as<sast::GenerateBlockSymbol>();
                if (!g.isUninstantiated)
                    for (const auto& m : g.members())
                        visit(m, depth + 1);
            }
            else if (sym.kind == sast::SymbolKind::GenerateBlockArray) {
                for (const auto* e : sym.as<sast::GenerateBlockArraySymbol>().entries)
                    visit(*e, depth + 1);
            }
        };
        for (const auto* top : root.topInstances)
            visit(*top, 0);
        std::set<const SyntaxNode*> hoisted;
        for (const auto& [loc, name] : undeclared) {
            for (const auto* body : bodies) {
                const auto* syn = body->getDefinition().getSyntax();
                if (!syn)
                    continue;
                const auto r = syn->sourceRange();
                const auto b = sm.getFullyOriginalLoc(r.start());
                const auto e = sm.getFullyOriginalLoc(r.end());
                if (b.buffer() != loc.buffer() || loc.offset() < b.offset() || loc.offset() > e.offset())
                    continue;
                // the declarations of the name in generate blocks directly in the module (either branch)
                std::vector<const sast::Symbol*> found;
                for (const auto& m : body->members())
                    if (m.kind == sast::SymbolKind::GenerateBlock)
                        if (const auto* d = m.as<sast::GenerateBlockSymbol>().find(name))
                            found.push_back(d);
                if (found.size() != 1 || !found.front()->getSyntax())
                    break;
                // the whole declaration (`reg [7:0] a, b;`) and the module item that holds the generate block
                const SyntaxNode* decl = found.front()->getSyntax()->parent;
                if (!decl || (decl->kind != SyntaxKind::DataDeclaration && decl->kind != SyntaxKind::NetDeclaration))
                    break;
                const SyntaxNode* item = decl;
                while (item->parent && item->parent->kind != SyntaxKind::ModuleDeclaration)
                    item = item->parent;
                if (!item->parent || !hoisted.insert(decl).second)
                    break;
                const auto dr = decl->sourceRange();
                const auto db = sm.getFullyOriginalLoc(dr.start());
                const auto de = sm.getFullyOriginalLoc(dr.end());
                const auto ib = sm.getFullyOriginalLoc(item->sourceRange().start());
                if (db.buffer() != loc.buffer() || de.buffer() != loc.buffer() || ib.buffer() != loc.buffer())
                    break;
                const auto t = sm.getSourceText(loc.buffer());
                std::string declText(t.substr(db.offset(), de.offset() - db.offset()));
                std::string blank = declText;
                for (auto& c : blank)
                    if (c != '\n' && c != '\r')
                        c = ' ';
                for (auto& c : declText)
                    if (c == '\n' || c == '\r')
                        c = ' ';
                const auto key = bufferKey(sm, loc.buffer());
                std::string names;
                const auto addNames = [&](const auto& declarators) {
                    for (const auto* d : declarators)
                        names += (names.empty() ? "" : ", ") + std::string(d->name.valueText());
                };
                if (decl->kind == SyntaxKind::DataDeclaration)
                    addNames(decl->as<DataDeclarationSyntax>().declarators);
                else
                    addNames(decl->as<NetDeclarationSyntax>().declarators);
                edits.push_back({key, db.offset(), blank.size(), blank, "", "", "", {}});
                edits.push_back({key, ib.offset(), 0, declText + " ", "FE-GENERATE-SCOPE",
                                 "'" + name + "' is used in the module but declared in a generate block: its "
                                              "declaration (" + names + ") is read in the module, as Quartus does",
                                 "declare it outside the generate block to make the design portable",
                                 convertLocation(sm, db)});
                break;
            }
        }
    }
    return edits;
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
    // every error is reported (and corrected by quartusCompatEdits), not only the first 64
    compOptions.errorLimit = 100000;
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

    // compiled once, or again with the corrections of quartusCompatEdits
    std::map<std::string, std::string> overrides;
    std::vector<SourceEdit> appliedEdits;
    const auto basePpOptions = ppOptions;
    std::unique_ptr<slang::SourceManager> sourceManagerOwner;
    std::unique_ptr<sast::Compilation> compilationOwner;
    for (int pass = 0;; ++pass) {
    ppOptions = basePpOptions;
    sourceManagerOwner = std::make_unique<slang::SourceManager>();
    auto& sourceManager = *sourceManagerOwner;
    CollectingDiagnosticSink silent;
    std::map<std::string, std::string> includedBy;
    preloadNonUtf8(paths, input.includeDirs, sourceManager, pass ? silent : sink,
                   svKeywordMapping ? &includedBy : nullptr, &overrides);
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
    // "// synthesis translate_off" ... "translate_on" and the vendor variants (as
    // the VHDL lexer): synthesis tools skip that code (ao486_MiSTer: a simulation-only
    // cpu_export instance in pipeline.v)
    slang::parsing::LexerOptions lexOptions;
    for (const char* prefix : {"synthesis", "synopsys", "pragma", "xilinx", "altera", "intel", "exemplar", "cadence"}) {
        lexOptions.commentHandlers[prefix]["translate_off"] = {slang::parsing::CommentHandler::TranslateOff,
                                                               "translate_on"};
        lexOptions.commentHandlers[prefix]["synthesis_off"] = {slang::parsing::CommentHandler::TranslateOff,
                                                               "synthesis_on"};
    }
    lexOptions.commentHandlers["rtl_synthesis"]["off"] = {slang::parsing::CommentHandler::TranslateOff, "on"};
    options.set(lexOptions);
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

    compilationOwner = std::make_unique<sast::Compilation>(options);
    compilationOwner->addSyntaxTree(*treeOrError);
    (void)compilationOwner->getRoot();
    if (pass < 3) {
        auto edits = quartusCompatEdits(compilationOwner->getAllDiagnostics(), **treeOrError, sourceManager,
                                        compilationOwner->getRoot());
        // per file, from the end: earlier offsets stay valid
        std::ranges::sort(edits, [](const SourceEdit& x, const SourceEdit& y) {
            return x.file != y.file ? x.file < y.file : x.offset > y.offset;
        });
        bool changed = false;
        for (const auto& e : edits) {
            auto it = overrides.find(e.file);
            if (it == overrides.end())
                for (auto buf : sourceManager.getAllBuffers())
                    if (bufferKey(sourceManager, buf) == e.file) {
                        auto t = std::string(sourceManager.getSourceText(buf));
                        while (!t.empty() && t.back() == '\0')
                            t.pop_back();
                        it = overrides.emplace(e.file, std::move(t)).first;
                        break;
                    }
            if (it == overrides.end() || e.offset + e.remove > it->second.size())
                continue;
            it->second.replace(e.offset, e.remove, e.insert);
            appliedEdits.push_back(e);
            changed = true;
        }
        if (changed)
            continue;
    }
    break;
    }
    auto& sourceManager = *sourceManagerOwner;
    auto& compilation = *compilationOwner;
    const auto& root = compilation.getRoot();
    for (const auto& e : appliedEdits)
        if (!e.code.empty())
            sink.report({e.code, Severity::Warning, e.message, e.where, {e.note}});

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
    ConstantBranches branches;
    if (std::ranges::any_of(slangDiags, [](const slang::Diagnostic& d) { return d.isError(); })) {
        ConstantBranchCollector collector(sourceManager, branches);
        root.visit(collector);
    }
    const bool hdlErrors = reportSlangDiagnostics(slangDiags, sourceManager, sink,
                                                  mixed ? &mixed->lenientModules : nullptr,
                                                  input.top.empty() && !mixed ? nullptr : &unused,
                                                  &compilation.getParseDiagnostics(), &branches);

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
