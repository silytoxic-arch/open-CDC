#pragma once
// Built-in models of FPGA vendor primitives (Xilinx UNISIM and XPM CDC
// macros, Intel synchronizers and dcfifo): Verilog modules with the
// register structure of the real cells, plus VHDL component packages
// (unisim.vcomponents, xpm.vcomponents, altera_mf.altera_mf_components).
// A model is added to a compilation only when the design instantiates it
// and does not define a module of that name itself.

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace opencdc::frontend {

class PrimitiveLibrary {
public:
    static const PrimitiveLibrary& instance();

    /// Exact module name for `name` (case-insensitive), if it is modelled.
    [[nodiscard]] std::optional<std::string> find(const std::string& name) const;
    /// Verilog text of the module and of the models it instantiates.
    [[nodiscard]] std::string verilogModel(const std::string& name) const;
    /// Clock generators (MMCM / PLL): their parameter lists are long and
    /// device specific, so the model is generated with the parameters the
    /// design actually uses. Every clock output is a clock derived from the
    /// input clock (synchronous to it and to each other).
    [[nodiscard]] bool isClockGenerator(const std::string& name) const;
    [[nodiscard]] std::string generatorModel(const std::string& name, const std::set<std::string>& params) const;
    /// Module names (as written) of the modelled primitives.
    [[nodiscard]] const std::vector<std::string>& names() const noexcept { return names_; }
    /// VHDL component packages: (library, file name, text).
    struct VhdlPackage {
        std::string library;
        std::string file;
        std::string text;
    };
    [[nodiscard]] const std::vector<VhdlPackage>& vhdlPackages() const noexcept { return packages_; }

private:
    PrimitiveLibrary();
    struct Model {
        std::string name;
        std::string text;
        std::vector<std::string> deps;
    };
    std::vector<Model> models_;
    std::vector<std::string> names_;
    std::vector<VhdlPackage> packages_;
    struct Generator {
        std::string name;
        std::string clockIn;
        std::vector<std::pair<std::string, int>> inputs;  ///< name, width
        std::vector<std::pair<std::string, int>> outputs;
    };
    std::vector<Generator> generators_;
};

namespace detail {
struct EmbeddedSource {
    const char* name;
    const char* const* parts;
    std::size_t count;
};
extern const EmbeddedSource kPrimitiveSources[];
extern const std::size_t kPrimitiveSourceCount;
} // namespace detail

} // namespace opencdc::frontend
