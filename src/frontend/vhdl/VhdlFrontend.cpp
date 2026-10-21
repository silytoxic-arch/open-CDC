// IFrontend implementation for VHDL: parse every file into its logical
// library, elaborate the top entity, build the Design IR.

#include "core/PathUtil.h"
#include "frontend/Frontend.h"
#include "frontend/vhdl/VhdlElab.h"
#include "frontend/vhdl/VhdlParser.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace opencdc {

namespace vhdl {

namespace {

std::string lowerCase(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

Result<bool> loadSources(const std::vector<std::filesystem::path>& paths, const std::vector<std::string>& libraries,
                         DiagnosticSink& sink, std::vector<SourceFileEntry>& files) {
    files.clear();
    files.reserve(paths.size());
    for (std::size_t i = 0; i < paths.size(); ++i) {
        const auto& p = paths[i];
        std::error_code ec;
        if (!std::filesystem::exists(p, ec))
            return Diagnostic{"FE-FILE-NOT-FOUND", Severity::Error, "source file not found: " + toUtf8(p), {}, {}};
        std::ifstream in(p, std::ios::binary);
        if (!in)
            return Diagnostic{"FE-FILE-READ", Severity::Error, "cannot read '" + toUtf8(p) + "'", {}, {}};
        std::ostringstream ss;
        ss << in.rdbuf();
        SourceFileEntry e;
        auto abs = std::filesystem::absolute(p, ec);
        e.path = toUtf8(ec ? p : abs.lexically_normal());
        e.library = i < libraries.size() && !libraries[i].empty() ? lowerCase(libraries[i]) : std::string("work");
        e.text = ss.str();
        files.push_back(std::move(e));
    }
    bool syntaxErrors = false;
    for (std::size_t i = 0; i < files.size(); ++i) {
        auto r = parseVhdl(files[i].text, static_cast<std::uint32_t>(i));
        for (const auto& m : r.messages) {
            SourceLocation loc;
            loc.file = files[i].path;
            loc.line = m.loc.line;
            loc.column = m.loc.col;
            sink.report({"VHDL-SYNTAX", m.error ? Severity::Error : Severity::Warning, m.message, loc, {}});
            syntaxErrors = syntaxErrors || m.error;
        }
        files[i].ast = std::move(r.file);
        for (auto& u : files[i].ast.units)
            u.library = files[i].library;
    }
    return !syntaxErrors;
}

} // namespace vhdl

namespace frontend {

namespace {

class VhdlFrontend final : public IFrontend {
public:
    Result<FrontendOutput> elaborate(const FrontendInput& input, DiagnosticSink& sink) override {
        if (input.sourceFiles.empty())
            return Diagnostic{"FE-NO-SOURCES", Severity::Error, "no HDL source files were given", {}, {}};
        std::vector<vhdl::SourceFileEntry> files;
        auto ok = vhdl::loadSources(input.sourceFiles, input.sourceLibraries, sink, files);
        if (!ok.ok())
            return ok.error();
        FrontendOutput output;
        output.summary.parsedFiles = input.sourceFiles;
        if (!ok.value())
            return output;

        vhdl::ElabOptions opts;
        opts.top = input.top;
        opts.generics = input.topParameters;
        vhdl::Elaborator elab(files, sink, opts);
        auto design = elab.run(output.summary);
        if (!elab.hadErrors())
            output.design = std::move(design);
        return output;
    }
};

} // namespace

std::unique_ptr<IFrontend> makeVhdlFrontend() {
    return std::make_unique<VhdlFrontend>();
}

} // namespace frontend

} // namespace opencdc
