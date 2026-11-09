#include "engine/FuseSocProject.h"

#include "core/PathUtil.h"
#include "engine/Yaml.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace opencdc::engine {

namespace {

std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    std::size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    return s.substr(b);
}

std::optional<std::string> readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::ostringstream t;
    t << in.rdbuf();
    return t.str();
}

/// "vendor:library:name" of a VLNV (a version and comparison operators dropped).
std::string vln(std::string v) {
    v = trim(v);
    while (!v.empty() && (v.front() == '>' || v.front() == '<' || v.front() == '=' || v.front() == '~' ||
                          v.front() == '^'))
        v.erase(v.begin());
    std::vector<std::string> parts;
    std::size_t s = 0;
    for (;;) {
        const auto c = v.find(':', s);
        parts.push_back(v.substr(s, c == std::string::npos ? std::string::npos : c - s));
        if (c == std::string::npos)
            break;
        s = c + 1;
    }
    if (parts.size() >= 3)
        return parts[0] + ":" + parts[1] + ":" + parts[2];
    return v;
}

std::string versionOf(const std::string& v) {
    std::size_t n = 0, pos = 0;
    for (; n < 3; ++n) {
        pos = v.find(':', pos);
        if (pos == std::string::npos)
            return {};
        ++pos;
    }
    return v.substr(pos);
}

struct Core {
    std::filesystem::path file;
    std::filesystem::path dir;
    std::string name; ///< full VLNV
    std::shared_ptr<const YamlNode> doc;
};

struct Flags {
    std::string tool;
    std::string target;
    bool toplevel = false;
    [[nodiscard]] bool has(std::string_view f) const {
        if (f == "is_toplevel")
            return toplevel;
        if (f.starts_with("tool_"))
            return f.substr(5) == tool;
        if (f.starts_with("target_"))
            return f.substr(7) == target;
        return false;
    }
};

/// The items of a list entry: "a", or "[!]flag ? (a b ...)" (nested), evaluated.
void expand(const std::string& text, const Flags& flags, std::vector<std::string>& out) {
    const std::string t = trim(text);
    const auto q = t.find('?');
    const auto open = t.find('(');
    if (q == std::string::npos || open == std::string::npos || open < q || t.back() != ')') {
        if (!t.empty())
            out.push_back(t);
        return;
    }
    std::string flag = trim(t.substr(0, q));
    bool negate = false;
    if (!flag.empty() && flag.front() == '!') {
        negate = true;
        flag = trim(flag.substr(1));
    }
    if (flags.has(flag) == negate)
        return;
    // the inner list: words separated by spaces outside parentheses; "f ? (x)" stays one item
    const std::string inner = t.substr(open + 1, t.size() - open - 2);
    std::vector<std::string> words;
    std::string w;
    int depth = 0;
    for (const char c : inner) {
        if (c == '(')
            ++depth;
        if (c == ')')
            --depth;
        if (std::isspace(static_cast<unsigned char>(c)) && depth == 0) {
            if (!w.empty())
                words.push_back(w);
            w.clear();
            continue;
        }
        w += c;
    }
    if (!w.empty())
        words.push_back(w);
    for (std::size_t i = 0; i < words.size(); ++i) {
        std::string item = words[i];
        // "f ? (x)", "f ?(x)", "f? (x)": joined
        while (i + 1 < words.size() && (item.back() == '?' || words[i + 1].front() == '?' ||
                                        (item.find('?') != std::string::npos && item.back() == '?'))) {
            item += " " + words[++i];
            if (item.back() == ')')
                break;
        }
        if (item.back() == '?' && i + 1 < words.size())
            item += " " + words[++i];
        expand(item, flags, out);
    }
}

std::vector<std::string> listOf(const YamlNode* n, const Flags& flags) {
    std::vector<std::string> out;
    if (!n)
        return out;
    if (n->isScalar()) {
        expand(n->scalar, flags, out);
        return out;
    }
    if (n->isSeq())
        for (const auto& it : n->items)
            if (it->isScalar())
                expand(it->scalar, flags, out);
    return out;
}

std::string stripComments(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n')
                ++i;
            out += '\n';
            continue;
        }
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/'))
                ++i;
            ++i;
            out += ' ';
            continue;
        }
        out += s[i];
    }
    return out;
}

class Importer {
public:
    Importer(const std::filesystem::path& top, std::string target)
        : top_(std::filesystem::absolute(top).lexically_normal()), wantedTarget_(std::move(target)) {}

    Result<ProjectConfig> run() {
        const auto text = readFile(top_);
        if (!text)
            return Diagnostic{"PRJ-OPEN", Severity::Error, "cannot open project file", {toUtf8(top_), 0, 0}, {}};
        std::string err;
        auto doc = parseYaml(*text, err);
        if (!doc || !(*doc)->isMap() || (*doc)->str("name").empty())
            return fail("not a FuseSoC core (CAPI2): " + (err.empty() ? std::string("no 'name'") : err));
        topCore_ = Core{top_, top_.parent_path(), (*doc)->str("name"), *doc};
        scanCores();

        const auto* targets = topCore_.doc->get("targets");
        std::string target = wantedTarget_;
        if (target.empty())
            target = targets && targets->get("synth") ? "synth" : "default";
        const YamlNode* t = targets ? targets->get(target) : nullptr;
        if (!t || !t->isMap())
            return Diagnostic{"PRJ-CORE-TARGET", Severity::Error,
                              "core '" + topCore_.name + "' has no target '" + target + "'", {toUtf8(top_), 0, 0}, {}};
        flags_.target = target;
        flags_.tool = t->str("default_tool", "vivado");
        cfg_.name = lastPart(topCore_.name);
        cfg_.top = t->str("toplevel");
        cfg_.importedFrom = top_;

        visit(topCore_, t, true);
        // the synthesis tools define SYNTHESIS (Vivado, Yosys and the flows built on it):
        // code for simulation only (lowRISC INC_ASSERT, assertion helpers reading child
        // signals) is left out, as in the tool's run
        static constexpr std::string_view kSynthTools[] = {"vivado", "yosys", "icestorm", "trellis", "apicula",
                                                           "nextpnr", "f4pga", "symbiflow", "vpr", "openlane"};
        const bool synthesisDefined =
            std::ranges::find(kSynthTools, flags_.tool) != std::end(kSynthTools) &&
            std::ranges::none_of(cfg_.defines, [](const frontend::MacroDefine& m) { return m.name == "SYNTHESIS"; });
        if (synthesisDefined)
            cfg_.defines.push_back({"SYNTHESIS", std::nullopt});
        if (cfg_.sources.empty())
            return fail("target '" + target + "' of core '" + topCore_.name + "' has no source files");
        if (sv_)
            cfg_.language = frontend::LanguageStd::SystemVerilog2017;
        cfg_.verilogFilesSystemVerilog = svInV_;

        if (!missingDeps_.empty())
            note("PRJ-CORE-MISSING-DEP", Severity::Warning,
                 std::to_string(missingDeps_.size()) + " dependency core(s) not found below " + toUtf8(top_.parent_path()),
                 {missingDeps_.begin(), missingDeps_.end()});
        if (!skippedTypes_.empty()) {
            std::vector<std::string> n;
            for (const auto& [k, c] : skippedTypes_)
                n.push_back(k + " (x" + std::to_string(c) + ")");
            note("PRJ-CORE-SKIPPED", Severity::Info, "files of types OpenCDC does not read", n);
        }
        note("PRJ-CORE-IMPORT", Severity::Info,
             "FuseSoC core imported: " + std::to_string(cfg_.sources.size()) + " source(s), " +
                 std::to_string(cfg_.constraints.size()) + " constraint file(s), top " +
                 (cfg_.top.empty() ? std::string("not set") : "'" + cfg_.top + "'") + " (target '" + target +
                 "', tool " + flags_.tool + ", " + std::to_string(visited_.size()) + " core(s)" +
                 (synthesisDefined ? std::string(", macro SYNTHESIS defined as by the tool") : std::string()) + ")",
             {});
        cfg_.importNotes = std::move(notes_);
        return cfg_;
    }

private:
    static std::string lastPart(const std::string& v) {
        const auto parts = vln(v);
        const auto c = parts.rfind(':');
        return c == std::string::npos ? parts : parts.substr(c + 1);
    }

    Diagnostic fail(std::string message) const {
        return Diagnostic{"PRJ-CORE-PARSE", Severity::Error, std::move(message), {toUtf8(top_), 0, 0}, {}};
    }

    void note(std::string code, Severity sev, std::string message, std::vector<std::string> notes) {
        notes_.push_back({std::move(code), sev, std::move(message), {toUtf8(top_), 0, 0}, std::move(notes)});
    }

    void scanCores() {
        std::error_code ec;
        const auto root = top_.parent_path();
        for (auto it = std::filesystem::recursive_directory_iterator(
                 root, std::filesystem::directory_options::skip_permission_denied, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (ec)
                break;
            const auto& p = it->path();
            const auto fname = toUtf8(p.filename());
            if (it->is_directory(ec)) {
                if (fname == ".git" || fname == "build" || fname == "node_modules")
                    it.disable_recursion_pending();
                continue;
            }
            if (lower(toUtf8(p.extension())) != ".core")
                continue;
            const auto text = readFile(p);
            if (!text)
                continue;
            std::string err;
            auto doc = parseYaml(*text, err);
            if (!doc || !(*doc)->isMap() || (*doc)->str("name").empty()) {
                unreadable_.push_back(toUtf8(p.lexically_relative(root)) + (err.empty() ? "" : ": " + err));
                continue;
            }
            Core c{std::filesystem::absolute(p).lexically_normal(), p.parent_path(), (*doc)->str("name"), *doc};
            auto& slot = cores_[vln(c.name)];
            // the highest version (by string, as written) wins
            if (!slot.doc || versionOf(c.name) > versionOf(slot.name))
                slot = std::move(c);
        }
        if (!unreadable_.empty())
            note("PRJ-CORE-UNREADABLE", Severity::Warning,
                 std::to_string(unreadable_.size()) + " core file(s) could not be read", unreadable_);
    }

    const Core* find(const std::string& v) const {
        const auto it = cores_.find(vln(v));
        return it == cores_.end() ? nullptr : &it->second;
    }

    /// The filesets of target `t` of `core`, in order.
    std::vector<std::string> filesetsOf(const YamlNode* t) const {
        auto fs = listOf(t ? t->get("filesets") : nullptr, flags_);
        const auto app = listOf(t ? t->get("filesets_append") : nullptr, flags_);
        fs.insert(fs.end(), app.begin(), app.end());
        return fs;
    }

    void visit(const Core& core, const YamlNode* target, bool top) {
        const std::string key = vln(core.name);
        if (!visited_.insert(key).second)
            return;
        Flags saved = flags_;
        flags_.toplevel = top;
        const auto* filesets = core.doc->get("filesets");
        const auto names = filesetsOf(target);
        // dependencies first
        for (const auto& fsName : names) {
            const auto* fs = filesets ? filesets->get(fsName) : nullptr;
            if (!fs)
                continue;
            for (const auto& dep : listOf(fs->get("depend"), flags_))
                visitDep(dep);
        }
        // generators of the target
        for (const auto& g : listOf(target ? target->get("generate") : nullptr, flags_))
            generate(core, g);
        // own files
        for (const auto& fsName : names) {
            const auto* fs = filesets ? filesets->get(fsName) : nullptr;
            if (fs)
                addFiles(core, *fs);
        }
        // parameters of the target
        parameters(core, target);
        flags_ = saved;
    }

    void visitDep(const std::string& dep) {
        const Core* c = find(dep);
        if (!c) {
            missingDeps_.insert(vln(dep));
            return;
        }
        const auto* targets = c->doc->get("targets");
        visit(*c, targets ? targets->get("default") : nullptr, false);
    }

    void addFiles(const Core& core, const YamlNode& fs) {
        const std::string defType = fs.str("file_type");
        const std::string defLib = fs.str("logical_name");
        const auto* files = fs.get("files");
        if (!files || !files->isSeq())
            return;
        for (const auto& item : files->items) {
            std::string name, type = defType, lib = defLib, includePath, copyto;
            bool include = false;
            if (item->isScalar()) {
                std::vector<std::string> expanded;
                expand(item->scalar, flags_, expanded);
                for (const auto& e : expanded)
                    addFile(core, e, type, lib, false, {}, {});
                continue;
            }
            if (item->isMap() && !item->entries.empty()) {
                name = item->entries.front().first;
                const auto& attrs = item->entries.front().second;
                if (attrs && attrs->isMap()) {
                    type = attrs->str("file_type", type);
                    lib = attrs->str("logical_name", lib);
                    include = lower(attrs->str("is_include_file")) == "true";
                    includePath = attrs->str("include_path");
                    copyto = attrs->str("copyto");
                }
                addFile(core, name, type, lib, include, includePath, copyto);
            }
        }
    }

    void addFile(const Core& core, const std::string& name, const std::string& type, const std::string& lib,
                 bool include, const std::string& includePath, const std::string& copyto) {
        auto p = (core.dir / fromUtf8(name)).lexically_normal();
        const bool hdl = type.starts_with("systemVerilogSource") || type.starts_with("verilogSource") ||
                         type.starts_with("vhdlSource");
        if (include && hdl) {
            std::filesystem::path dir = includePath.empty() ? p.parent_path() : (core.dir / fromUtf8(includePath));
            if (!copyto.empty()) {
                // the file is read under another name (ibex: copyto multdiv_check.svh)
                const auto dst = generatedDir() / fromUtf8(copyto);
                std::filesystem::create_directories(dst.parent_path());
                std::error_code ec;
                std::filesystem::copy_file(p, dst, std::filesystem::copy_options::overwrite_existing, ec);
                dir = dst.parent_path();
            }
            dir = dir.lexically_normal();
            if (std::ranges::find(cfg_.includeDirs, dir) == cfg_.includeDirs.end())
                cfg_.includeDirs.push_back(dir);
            return;
        }
        if (hdl) {
            if (std::ranges::find(cfg_.sources, p) != cfg_.sources.end())
                return;
            cfg_.sources.push_back(p);
            cfg_.sourceLibraries.push_back(type.starts_with("vhdlSource") ? lib : std::string());
            if (type.starts_with("systemVerilogSource")) {
                sv_ = true;
                if (lower(toUtf8(p.extension())) == ".v")
                    svInV_ = true;
            }
            return;
        }
        if (type == "xdc" || type == "sdc") {
            if (std::ranges::find(cfg_.constraints, p) == cfg_.constraints.end())
                cfg_.constraints.push_back(p);
            return;
        }
        ++skippedTypes_[type.empty() ? std::string("(no file type)") : type];
    }

    void parameters(const Core& core, const YamlNode* target) {
        const auto* defs = core.doc->get("parameters");
        for (const auto& p : listOf(target ? target->get("parameters") : nullptr, flags_)) {
            const auto eq = p.find('=');
            const std::string name = trim(p.substr(0, eq));
            const auto* def = defs ? defs->get(name) : nullptr;
            if (!def || !def->isMap())
                continue;
            std::optional<std::string> value;
            if (eq != std::string::npos)
                value = trim(p.substr(eq + 1));
            else if (const auto* d = def->get("default"); d && d->isScalar())
                value = d->scalar;
            if (!value)
                continue;
            const std::string type = def->str("paramtype");
            const std::string datatype = def->str("datatype");
            if (type == "vlogdefine") {
                std::erase_if(cfg_.defines, [&](const frontend::MacroDefine& m) { return m.name == name; });
                cfg_.defines.push_back({name, *value});
            }
            else if (type == "vlogparam" || type == "generic") {
                std::string v = *value;
                if (datatype == "str")
                    v = "\"" + v + "\"";
                else if (datatype == "bool")
                    v = lower(v) == "true" ? "1" : lower(v) == "false" ? "0" : v;
                std::erase_if(cfg_.parameters, [&](const auto& e) { return e.first == name; });
                cfg_.parameters.emplace_back(name, v);
            }
        }
    }

    std::filesystem::path generatedDir() {
        if (genDir_.empty()) {
            const auto h = std::hash<std::string>{}(toUtf8(top_) + "@" + flags_.target);
            genDir_ = std::filesystem::temp_directory_path() / ("opencdc-fusesoc-" + std::to_string(h % 100000000));
            std::error_code ec;
            std::filesystem::create_directories(genDir_, ec);
        }
        return genDir_;
    }

    void writeGenerated(const std::filesystem::path& p, const std::string& text) {
        const auto old = readFile(p);
        if (!old || *old != text) {
            std::ofstream out(p, std::ios::binary);
            out << text;
        }
        if (std::ranges::find(cfg_.sources, p) == cfg_.sources.end()) {
            cfg_.sources.push_back(p);
            cfg_.sourceLibraries.emplace_back();
            sv_ = true;
        }
    }

    // ---- generators ----

    void generate(const Core& core, const std::string& name) {
        const auto* gens = core.doc->get("generate");
        const auto* g = gens ? gens->get(name) : nullptr;
        if (!g || !g->isMap())
            return;
        const std::string generator = g->str("generator");
        const auto* params = g->get("parameters");
        if (generator == "primgen") {
            primgen(core, params);
            return;
        }
        note("PRJ-CORE-GENERATOR", Severity::Warning,
             "generator '" + generator + "' of core '" + core.name + "' is not run: the files it generates are missing",
             {"run 'fusesoc run --setup' and add the generated files, or describe them in an OpenCDC project"});
    }

    /// lowRISC's primgen (vendor/lowrisc_ip/ip/prim/util/primgen.py): prim_pkg.sv with one
    /// implementation per technology library, and the abstract primitive prim_<name> that
    /// instantiates the library's implementation selected by `PRIM_DEFAULT_IMPL.
    void primgen(const Core& core, const YamlNode* params) {
        // the primitive cores: lowrisc:prim_<techlib>:<name>
        auto techlibsOf = [&](const std::string& prim) {
            std::set<std::string> libs;
            for (const auto& [k, c] : cores_) {
                const auto a = k.find(':');
                const auto b = k.find(':', a + 1);
                if (a == std::string::npos || b == std::string::npos)
                    continue;
                const std::string vendor = k.substr(0, a), library = k.substr(a + 1, b - a - 1),
                                  pname = k.substr(b + 1);
                if (vendor == "lowrisc" && library.starts_with("prim_") && (prim.empty() || pname == prim))
                    libs.insert(library.substr(5));
            }
            return libs;
        };
        const auto enumName = [](std::string lib) {
            if (!lib.empty())
                lib[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(lib[0])));
            for (std::size_t i = 1; i < lib.size(); ++i)
                lib[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(lib[i])));
            return "Impl" + lib;
        };
        const std::string action = params ? params->str("action") : std::string();
        if (action == "generate_prim_pkg") {
            const auto libs = techlibsOf("");
            std::string body = "// Generated by OpenCDC (FuseSoC generator primgen of " + core.name +
                               ")\npackage prim_pkg;\n  typedef enum integer {\n    ImplGeneric";
            for (const auto& l : libs)
                if (l != "generic")
                    body += ",\n    " + enumName(l);
            body += "\n  } impl_e;\nendpackage : prim_pkg\n";
            writeGenerated(generatedDir() / "prim_pkg.sv", body);
            primgenDone_.push_back("prim_pkg");
            return;
        }
        const std::string prim = params ? params->str("prim_name") : std::string();
        if (prim.empty())
            return;
        const auto libs = techlibsOf(prim);
        if (!libs.contains("generic")) {
            note("PRJ-CORE-GENERATOR", Severity::Warning,
                 "primgen: no generic implementation of primitive '" + prim + "' (lowrisc:prim_generic:" + prim + ")", {});
            return;
        }
        // the technology library cores (and prim_pkg) come first
        if (const Core* pkg = find("lowrisc:prim:prim_pkg"))
            visitDep(pkg->name);
        for (const auto& l : libs)
            visitDep("lowrisc:prim_" + l + ":" + prim);
        // the generic implementation's module header
        const std::string module = "prim_generic_" + prim;
        std::optional<std::string> text;
        for (const auto& s : cfg_.sources)
            if (toUtf8(s.filename()) == module + ".sv")
                text = readFile(s);
        if (!text) {
            note("PRJ-CORE-GENERATOR", Severity::Warning, "primgen: " + module + ".sv not found", {});
            return;
        }
        const std::string src = stripComments(*text);
        static const std::regex hdrRe(
            R"((?:\s|^)module\s+(?:(?:static|automatic)\s+)?(\w+)\s*((?:import\s+[^;]+;)+)?\s*(?:#\s*\(([^;]+)\))?\s*\(\s*([^;]+)\s*\)\s*;)");
        std::smatch m;
        std::string imports, paramList, ports;
        bool found = false;
        for (auto it = std::sregex_iterator(src.begin(), src.end(), hdrRe); it != std::sregex_iterator(); ++it)
            if ((*it)[1].str() == module) {
                imports = (*it)[2].str();
                paramList = (*it)[3].str();
                ports = trim((*it)[4].str());
                found = true;
                break;
            }
        if (!found) {
            note("PRJ-CORE-GENERATOR", Severity::Warning, "primgen: the header of " + module + " could not be read", {});
            return;
        }
        static const std::regex paramRe(R"(parameter\s+(?:[a-zA-Z0-9_\]\[:\s\$-]+\s+)?(\w+)(?:\s*=\s*[^,;]+)?)");
        std::set<std::string> names;
        for (auto it = std::sregex_iterator(paramList.begin(), paramList.end(), paramRe); it != std::sregex_iterator();
             ++it)
            names.insert((*it)[1].str());
        const auto instance = [&](const std::string& lib) {
            std::string s = "    prim_" + lib + "_" + prim;
            if (names.empty())
                s += " u_impl_" + lib + " (\n";
            else {
                s += " #(\n";
                bool first = true;
                for (const auto& n : names) {
                    s += (first ? "" : ",\n") + std::string("      .") + n + "(" + n + ")";
                    first = false;
                }
                s += "\n    ) u_impl_" + lib + " (\n";
            }
            return s + "      .*\n    );\n";
        };
        std::vector<std::string> order;
        for (const auto& l : libs)
            if (l != "generic")
                order.push_back(l);
        std::string out = "// Generated by OpenCDC (FuseSoC generator primgen of " + core.name + ")\n";
        if (order.size() + 1 > 1 && !order.empty())
            out += "`ifndef PRIM_DEFAULT_IMPL\n  `define PRIM_DEFAULT_IMPL prim_pkg::ImplGeneric\n`endif\n";
        out += "module prim_" + prim + "\n" + imports + "\n#(\n" + paramList + "\n) (\n  " + ports + "\n);\n";
        if (!order.empty())
            out += "  localparam prim_pkg::impl_e Impl = `PRIM_DEFAULT_IMPL;\n";
        out += "\n";
        if (order.empty()) {
            out += "  if (1) begin : gen_generic\n" + instance("generic") + "\n  end";
        }
        else {
            order.push_back("generic");
            for (std::size_t i = 0; i < order.size(); ++i) {
                const bool last = i + 1 == order.size();
                if (i > 0)
                    out += "else ";
                if (!last)
                    out += "if (Impl == prim_pkg::" + enumName(order[i]) + ") ";
                out += "begin : gen_" + order[i] + "\n" + instance(order[i]) + "end";
                if (!last)
                    out += " ";
            }
        }
        out += "\n\nendmodule\n";
        writeGenerated(generatedDir() / ("prim_" + prim + ".sv"), out);
        primgenDone_.push_back("prim_" + prim);
    }

    std::filesystem::path top_;
    std::string wantedTarget_;
    Core topCore_;
    std::map<std::string, Core> cores_;
    std::set<std::string> visited_;
    std::set<std::string> missingDeps_;
    std::vector<std::string> unreadable_;
    std::map<std::string, std::size_t> skippedTypes_;
    std::vector<std::string> primgenDone_;
    Flags flags_;
    ProjectConfig cfg_;
    bool sv_ = false;
    bool svInV_ = false;
    std::filesystem::path genDir_;
    std::vector<Diagnostic> notes_;

public:
    [[nodiscard]] const std::vector<std::string>& generated() const { return primgenDone_; }
};

} // namespace

bool isFuseSocCoreFile(const std::filesystem::path& file) { return lower(toUtf8(file.extension())) == ".core"; }

Result<ProjectConfig> projectFromCoreFile(const std::filesystem::path& file, const std::string& target) {
    Importer imp(file, target);
    auto r = imp.run();
    if (r && !imp.generated().empty()) {
        auto cfg = std::move(r).value();
        std::vector<std::string> names = imp.generated();
        cfg.importNotes.insert(cfg.importNotes.end() - 1,
                               Diagnostic{"PRJ-CORE-PRIMGEN", Severity::Info,
                                          std::to_string(names.size()) +
                                              " file(s) of lowRISC's primgen generator written by OpenCDC "
                                              "(prim_pkg and abstract primitives)",
                                          {toUtf8(std::filesystem::absolute(file).lexically_normal()), 0, 0},
                                          std::move(names)});
        return cfg;
    }
    return r;
}

} // namespace opencdc::engine
