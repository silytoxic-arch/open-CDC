// IFrontend implementation for VHDL: parse every file into its logical
// library, elaborate the top entity, build the Design IR.

#include "core/PathUtil.h"
#include "frontend/Frontend.h"
#include "frontend/vhdl/VhdlElab.h"
#include "frontend/vhdl/VhdlParser.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace opencdc::frontend {

namespace {

std::string lowerCase(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

class VhdlFrontend final : public IFrontend {
public:
    Result<FrontendOutput> elaborate(const FrontendInput& input, DiagnosticSink& sink) override {
        if (input.sourceFiles.empty())
            return Diagnostic{"FE-NO-SOURCES", Severity::Error, "no HDL source files were given", {}, {}};
        std::vector<vhdl::SourceFileEntry> files;
        files.reserve(input.sourceFiles.size());
        FrontendOutput output;
        for (std::size_t i = 0; i < input.sourceFiles.size(); ++i) {
            const auto& p = input.sourceFiles[i];
            std::error_code ec;
            if (!std::filesystem::exists(p, ec))
                return Diagnostic{"FE-FILE-NOT-FOUND", Severity::Error, "source file not found: " + toUtf8(p), {}, {}};
            std::ifstream in(p, std::ios::binary);
            if (!in)
                return Diagnostic{"FE-FILE-READ", Severity::Error, "cannot read '" + toUtf8(p) + "'", {}, {}};
            std::ostringstream ss;
            ss << in.rdbuf();
            vhdl::SourceFileEntry e;
            auto abs = std::filesystem::absolute(p, ec);
            e.path = toUtf8(ec ? p : abs.lexically_normal());
            e.library = i < input.sourceLibraries.size() && !input.sourceLibraries[i].empty()
                            ? lowerCase(input.sourceLibraries[i])
                            : std::string("work");
            e.text = ss.str();
            files.push_back(std::move(e));
            output.summary.parsedFiles.push_back(p);
        }
        bool syntaxErrors = false;
        for (std::size_t i = 0; i < files.size(); ++i) {
            auto r = vhdl::parseVhdl(files[i].text, static_cast<std::uint32_t>(i));
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
        if (syntaxErrors)
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

} // namespace opencdc::frontend
