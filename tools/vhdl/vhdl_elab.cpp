// Development tool: elaborates VHDL files and dumps the Design IR.
//   opencdc-vhdl-elab [--top name] [-g NAME=VALUE]... [--lib name] file.vhd ... [--lib name file ...] [--quiet]
#include "core/Diagnostic.h"
#include "frontend/Frontend.h"
#include "ir/Dump.h"

#include <chrono>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    opencdc::frontend::FrontendInput in;
    std::string lib = "work";
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--top" && i + 1 < argc)
            in.top = argv[++i];
        else if (a == "--lib" && i + 1 < argc)
            lib = argv[++i];
        else if (a == "-g" && i + 1 < argc) {
            const std::string g = argv[++i];
            const auto eq = g.find('=');
            if (eq != std::string::npos)
                in.topParameters.emplace_back(g.substr(0, eq), g.substr(eq + 1));
        }
        else if (a == "--quiet")
            quiet = true;
        else {
            in.sourceFiles.emplace_back(a);
            in.sourceLibraries.push_back(lib);
        }
    }
    opencdc::CollectingDiagnosticSink sink;
    const auto t0 = std::chrono::steady_clock::now();
    auto fe = opencdc::frontend::makeVhdlFrontend();
    auto r = fe->elaborate(in, sink);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    for (const auto& d : sink.diagnostics())
        std::cout << opencdc::format(d) << "\n";
    if (!r.ok()) {
        std::cout << opencdc::format(r.error()) << "\n";
        return 2;
    }
    const auto& out = r.value();
    if (out.design) {
        if (!quiet)
            std::cout << opencdc::ir::dumpDesign(*out.design);
        const auto st = out.design->stats();
        std::cout << "bodies=" << st.uniqueBodies << " instances=" << st.instances << " registers=" << st.registers
                  << " regbits=" << st.registerBits << " latches=" << st.latches << " cells=" << st.cells
                  << " unsupported=" << st.unsupported << "\n";
    }
    else {
        std::cout << "no design\n";
    }
    std::cout << "time " << s << " s\n";
    return sink.hasErrors() || !out.design ? 1 : 0;
}
