#include "frontend/Primitives.h"

#include <algorithm>
#include <cctype>
#include <regex>

namespace opencdc::frontend {

namespace {

std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

const PrimitiveLibrary& PrimitiveLibrary::instance() {
    static const PrimitiveLibrary lib;
    return lib;
}

PrimitiveLibrary::PrimitiveLibrary() {
    for (std::size_t i = 0; i < detail::kPrimitiveSourceCount; ++i) {
        const auto& src = detail::kPrimitiveSources[i];
        std::string text;
        for (std::size_t k = 0; k < src.count; ++k)
            text += src.parts[k];
        const std::string file = src.name;
        if (file.ends_with(".vhd")) {
            std::string libName = file.starts_with("unisim")  ? "unisim"
                                  : file.starts_with("xpm")   ? "xpm"
                                                              : "altera_mf";
            packages_.push_back({libName, file, std::move(text)});
            continue;
        }
        // split a Verilog file into its modules
        static const std::regex moduleRe(R"(^module\s+([A-Za-z_][A-Za-z0-9_]*))");
        std::size_t pos = 0;
        while ((pos = text.find("\nmodule ", pos)) != std::string::npos) {
            const std::size_t start = pos + 1;
            const std::size_t end = text.find("endmodule", start);
            if (end == std::string::npos)
                break;
            const std::string body = text.substr(start, end + 9 - start) + "\n";
            std::smatch m;
            if (std::regex_search(body, m, moduleRe))
                models_.push_back({m[1].str(), body, {}});
            pos = end;
        }
    }
    for (const auto& m : models_)
        names_.push_back(m.name);
    // dependencies: other modelled modules instantiated by a model
    for (auto& m : models_)
        for (const auto& o : models_)
            if (&o != &m && std::regex_search(m.text, std::regex("(^|[^A-Za-z0-9_$])" + o.name +
                                                                 "(\\s*#|\\s+[A-Za-z_][A-Za-z0-9_]*\\s*\\()")))
                m.deps.push_back(o.name);
}

std::optional<std::string> PrimitiveLibrary::find(const std::string& name) const {
    for (const auto& m : models_)
        if (m.name == name)
            return m.name;
    const std::string key = lower(name);
    for (const auto& m : models_)
        if (lower(m.name) == key)
            return m.name;
    return std::nullopt;
}

std::string PrimitiveLibrary::verilogModel(const std::string& name) const {
    std::string out;
    std::vector<std::string> done;
    std::vector<std::string> work{name};
    while (!work.empty()) {
        const std::string n = work.back();
        work.pop_back();
        if (std::ranges::find(done, n) != done.end())
            continue;
        done.push_back(n);
        for (const auto& m : models_)
            if (m.name == n) {
                out += m.text;
                for (const auto& d : m.deps)
                    work.push_back(d);
            }
    }
    return out;
}

} // namespace opencdc::frontend
