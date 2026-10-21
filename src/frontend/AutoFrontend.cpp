// Default frontend: chooses the language frontend by source file extension
// (.vhd / .vhdl: VHDL, everything else: Verilog / SystemVerilog via slang).

#include "frontend/Frontend.h"

#include <algorithm>
#include <cctype>

namespace opencdc::frontend {

bool isVhdlFile(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".vhd" || ext == ".vhdl" || ext == ".vho";
}

namespace {

class AutoFrontend final : public IFrontend {
public:
    Result<FrontendOutput> elaborate(const FrontendInput& input, DiagnosticSink& sink) override {
        const auto vhdl = std::ranges::count_if(input.sourceFiles, isVhdlFile);
        if (vhdl == 0)
            return makeSlangFrontend()->elaborate(input, sink);
        if (static_cast<std::size_t>(vhdl) == input.sourceFiles.size())
            return makeVhdlFrontend()->elaborate(input, sink);
        return Diagnostic{"FE-MIXED", Severity::Error,
                          "mixed VHDL / Verilog projects are not supported yet", {}, {}};
    }
};

} // namespace

std::unique_ptr<IFrontend> makeDefaultFrontend() {
    return std::make_unique<AutoFrontend>();
}

} // namespace opencdc::frontend
