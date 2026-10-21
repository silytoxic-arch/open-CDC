// Development tool: parses VHDL files and prints syntax errors.
//   opencdc-vhdl-parse file.vhd...   (exit code 1 if any file has errors)
#include "frontend/vhdl/VhdlParser.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

int main(int argc, char** argv) {
    int bad = 0;
    std::size_t units = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 1; i < argc; ++i) {
        std::ifstream in(argv[i], std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const auto r = opencdc::vhdl::parseVhdl(ss.str(), 0);
        units += r.file.units.size();
        for (const auto& m : r.messages)
            std::cout << argv[i] << ":" << m.loc.line << ":" << m.loc.col << ": " << (m.error ? "error" : "warning")
                      << ": " << m.message << "\n";
        bad += r.hasErrors() ? 1 : 0;
    }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::cout << (argc - 1) << " file(s), " << units << " design unit(s), " << bad << " with errors, " << s << " s\n";
    return bad ? 1 : 0;
}
