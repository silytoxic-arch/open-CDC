#include "engine/VivadoProject.h"

#include "core/PathUtil.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>

namespace opencdc::engine {

namespace {

// ---- a small XML reader (elements and attributes; text is not needed) ----

struct XmlNode {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<std::unique_ptr<XmlNode>> children;

    [[nodiscard]] std::string attr(std::string_view n) const {
        for (const auto& [k, v] : attrs)
            if (k == n)
                return v;
        return {};
    }
    [[nodiscard]] std::vector<const XmlNode*> all(std::string_view n) const {
        std::vector<const XmlNode*> out;
        for (const auto& c : children)
            if (c->name == n)
                out.push_back(c.get());
        return out;
    }
    [[nodiscard]] const XmlNode* first(std::string_view n) const {
        for (const auto& c : children)
            if (c->name == n)
                return c.get();
        return nullptr;
    }
};

std::string decodeEntities(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        const auto semi = s.find(';', i);
        if (semi == std::string_view::npos) {
            out += s[i];
            continue;
        }
        const auto e = s.substr(i + 1, semi - i - 1);
        if (e == "amp")
            out += '&';
        else if (e == "lt")
            out += '<';
        else if (e == "gt")
            out += '>';
        else if (e == "quot")
            out += '"';
        else if (e == "apos")
            out += '\'';
        else if (!e.empty() && e[0] == '#') {
            const bool hex = e.size() > 1 && (e[1] == 'x' || e[1] == 'X');
            const auto code = std::strtoul(std::string(e.substr(hex ? 2 : 1)).c_str(), nullptr, hex ? 16 : 10);
            if (code < 0x80)
                out += static_cast<char>(code);
        }
        else {
            out.append(s.substr(i, semi - i + 1));
        }
        i = semi;
    }
    return out;
}

/// The element tree of `text`, or nullptr when it is not well formed.
std::unique_ptr<XmlNode> parseXml(std::string_view text) {
    auto root = std::make_unique<XmlNode>();
    std::vector<XmlNode*> stack{root.get()};
    std::size_t i = 0;
    while ((i = text.find('<', i)) != std::string_view::npos) {
        if (text.substr(i, 4) == "<!--") {
            const auto e = text.find("-->", i);
            if (e == std::string_view::npos)
                return nullptr;
            i = e + 3;
            continue;
        }
        if (text.substr(i, 2) == "<?" || text.substr(i, 2) == "<!") {
            const auto e = text.find('>', i);
            if (e == std::string_view::npos)
                return nullptr;
            i = e + 1;
            continue;
        }
        // the end of the tag: '>' outside quoted attribute values
        std::size_t e = i + 1;
        char quote = 0;
        for (; e < text.size(); ++e) {
            if (quote) {
                if (text[e] == quote)
                    quote = 0;
            }
            else if (text[e] == '"' || text[e] == '\'')
                quote = text[e];
            else if (text[e] == '>')
                break;
        }
        if (e >= text.size())
            return nullptr;
        std::string_view tag = text.substr(i + 1, e - i - 1);
        i = e + 1;
        if (!tag.empty() && tag[0] == '/') {
            if (stack.size() < 2)
                return nullptr;
            stack.pop_back();
            continue;
        }
        const bool selfClosing = !tag.empty() && tag.back() == '/';
        if (selfClosing)
            tag.remove_suffix(1);
        auto node = std::make_unique<XmlNode>();
        std::size_t p = 0;
        while (p < tag.size() && !std::isspace(static_cast<unsigned char>(tag[p])))
            ++p;
        node->name = std::string(tag.substr(0, p));
        while (p < tag.size()) {
            while (p < tag.size() && std::isspace(static_cast<unsigned char>(tag[p])))
                ++p;
            const auto eq = tag.find('=', p);
            if (eq == std::string_view::npos)
                break;
            std::string key(tag.substr(p, eq - p));
            while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back())))
                key.pop_back();
            auto q = eq + 1;
            while (q < tag.size() && std::isspace(static_cast<unsigned char>(tag[q])))
                ++q;
            if (q >= tag.size() || (tag[q] != '"' && tag[q] != '\''))
                return nullptr;
            const auto close = tag.find(tag[q], q + 1);
            if (close == std::string_view::npos)
                return nullptr;
            node->attrs.emplace_back(std::move(key), decodeEntities(tag.substr(q + 1, close - q - 1)));
            p = close + 1;
        }
        XmlNode* raw = node.get();
        stack.back()->children.push_back(std::move(node));
        if (!selfClosing)
            stack.push_back(raw);
    }
    if (stack.size() != 1)
        return nullptr;
    return root;
}

// ---- the project ----

std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string extensionOf(const std::filesystem::path& p) { return lower(toUtf8(p.extension())); }

/// The options of a <Config> or <Configuration> element: name -> values in order.
std::multimap<std::string, std::string> optionsOf(const XmlNode* config) {
    std::multimap<std::string, std::string> out;
    if (config)
        for (const auto* o : config->all("Option"))
            out.emplace(o->attr("Name"), o->attr("Val"));
    return out;
}

std::vector<std::string> values(const std::multimap<std::string, std::string>& opts, const std::string& name) {
    std::vector<std::string> out;
    const auto [b, e] = opts.equal_range(name);
    for (auto it = b; it != e; ++it)
        out.push_back(it->second);
    return out;
}

struct XprFile {
    std::string pathText; ///< as written
    std::filesystem::path path;
    std::multimap<std::string, std::string> attrs; ///< FileInfo attributes
    [[nodiscard]] std::vector<std::string> all(const std::string& n) const { return values(attrs, n); }
    [[nodiscard]] std::string one(const std::string& n) const {
        const auto it = attrs.find(n);
        return it == attrs.end() ? std::string() : it->second;
    }
    [[nodiscard]] bool enabled() const { return one("AutoDisabled") != "1" && one("IsEnabled") != "0"; }
    [[nodiscard]] bool usedIn(std::string_view step) const {
        const auto used = all("UsedIn");
        return used.empty() || std::ranges::find(used, step) != used.end();
    }
};

class Importer {
public:
    Importer(const std::filesystem::path& xprFile)
        : xpr_(std::filesystem::absolute(xprFile).lexically_normal()), dir_(xpr_.parent_path()),
          name_(toUtf8(xpr_.stem())) {}

    Result<ProjectConfig> run(std::string_view text) {
        const auto doc = parseXml(text);
        const XmlNode* project = doc ? doc->first("Project") : nullptr;
        if (!project)
            return fail("not a Vivado project (.xpr): no <Project> element");
        const auto projOpts = optionsOf(project->first("Configuration"));
        vars_ = {{"PPRDIR", toUtf8(dir_)},
                 {"PSRCDIR", toUtf8(dir_ / (name_ + ".srcs"))},
                 {"PGENDIR", toUtf8(dir_ / (name_ + ".gen"))},
                 {"PCACHEDIR", toUtf8(dir_ / (name_ + ".cache"))},
                 {"PRUNDIR", toUtf8(dir_ / (name_ + ".runs"))},
                 {"PIPUSERFILESDIR", toUtf8(dir_ / (name_ + ".ip_user_files"))}};
        if (const auto ipu = values(projOpts, "IPUserFilesDir"); !ipu.empty() && !ipu.front().empty())
            vars_["PIPUSERFILESDIR"] = toUtf8(expand(ipu.front()));
        defaultLib_ = values(projOpts, "DefaultLib").empty() ? "xil_defaultlib" : values(projOpts, "DefaultLib").front();

        // the file sets of the current synthesis run
        std::string srcSet, constrSet;
        if (const auto* runs = project->first("Runs"))
            for (const auto* r : runs->all("Run"))
                if (r->attr("Type").find("Synth") != std::string::npos &&
                    (r->attr("State") == "current" || (srcSet.empty() && r->attr("Id") == "synth_1"))) {
                    srcSet = r->attr("SrcSet");
                    constrSet = r->attr("ConstrsSet");
                    if (r->attr("State") == "current")
                        break;
                }
        const XmlNode* filesets = project->first("FileSets");
        std::vector<const XmlNode*> sets = filesets ? filesets->all("FileSet") : std::vector<const XmlNode*>{};
        const auto find = [&](const std::string& name, const std::string& type) -> const XmlNode* {
            for (const auto* s : sets)
                if (!name.empty() && s->attr("Name") == name)
                    return s;
            for (const auto* s : sets)
                if (s->attr("Type") == type)
                    return s;
            return nullptr;
        };
        const XmlNode* src = find(srcSet, "DesignSrcs");
        if (!src)
            return fail("the project has no design sources (no DesignSrcs file set)");
        const XmlNode* constrs = find(constrSet, "Constrs");

        ProjectConfig cfg;
        cfg.name = name_;
        cfg.importedFrom = xpr_;

        // sources, then the IP of their own file sets
        const auto srcFiles = filesOf(src);
        std::vector<XprFile> ipFiles;
        for (const auto* s : sets)
            if (s->attr("Type") == "BlockSrcs")
                for (auto& f : filesOf(s))
                    ipFiles.push_back(std::move(f));
        addSources(cfg, srcFiles, ipFiles);

        // top, defines, include directories, generics of the source set
        const auto srcOpts = optionsOf(src->first("Config"));
        if (const auto top = values(srcOpts, "TopModule"); !top.empty())
            cfg.top = top.front();
        for (const auto& d : values(srcOpts, "VerilogDefine"))
            for (const auto& item : splitList(d)) {
                const auto eq = item.find('=');
                if (eq == std::string::npos)
                    cfg.defines.push_back({item, std::nullopt});
                else
                    cfg.defines.push_back({item.substr(0, eq), item.substr(eq + 1)});
            }
        for (const auto& d : values(srcOpts, "VerilogDir"))
            for (const auto& item : splitList(d))
                addIncludeDir(cfg, expand(item));
        for (const auto& g : values(srcOpts, "Generic"))
            for (const auto& item : splitList(g))
                if (const auto eq = item.find('='); eq != std::string::npos)
                    cfg.parameters.emplace_back(item.substr(0, eq), item.substr(eq + 1));

        if (constrs)
            addConstraints(cfg, filesOf(constrs));
        // SystemVerilog sources (or headers) select IEEE 1800-2017, as for an OpenCDC project
        if (sv_ || std::ranges::any_of(cfg.sources, [](const auto& f) { return extensionOf(f) == ".sv"; }))
            cfg.language = frontend::LanguageStd::SystemVerilog2017;

        for (const auto& repo : values(projOpts, "IPRepoPath")) {
            const auto p = expand(repo);
            std::error_code ec;
            if (std::filesystem::is_directory(p, ec))
                cfg.ipRepositories.push_back(p);
            else
                note("PRJ-XPR-MISSING", Severity::Warning, "IP repository not found: " + repo,
                     {"resolved to " + toUtf8(p)});
        }

        if (!skipped_.empty())
            note("PRJ-XPR-SKIPPED", Severity::Info,
                 std::to_string(skipped_.size()) + " file(s) of the project are not read by OpenCDC (memory "
                                                   "initialization, IP-internal and simulation files)",
                 skipped_);
        note("PRJ-XPR-IMPORT", Severity::Info,
             "Vivado project imported: " + std::to_string(cfg.sources.size()) + " source(s), " +
                 std::to_string(cfg.constraints.size()) + " constraint file(s), top " +
                 (cfg.top.empty() ? std::string("not set") : "'" + cfg.top + "'") + " (source set '" +
                 src->attr("Name") + "'" + (constrs ? ", constraint set '" + constrs->attr("Name") + "'" : "") + ")",
             {});
        cfg.importNotes = std::move(notes_);
        return cfg;
    }

private:
    Diagnostic fail(std::string message) const {
        Diagnostic d{"PRJ-XPR-PARSE", Severity::Error, std::move(message), {}, {}};
        d.location.file = toUtf8(xpr_);
        return d;
    }

    void note(std::string code, Severity sev, std::string message, std::vector<std::string> notes) {
        Diagnostic d{std::move(code), sev, std::move(message), {}, std::move(notes)};
        d.location.file = toUtf8(xpr_);
        notes_.push_back(std::move(d));
    }

    /// `$PPRDIR/x` etc. resolved; relative paths are relative to the project directory.
    std::filesystem::path expand(const std::string& text) const {
        std::string s = text;
        for (std::size_t i = 0; (i = s.find('$', i)) != std::string::npos;) {
            std::size_t e = i + 1;
            while (e < s.size() && (std::isalnum(static_cast<unsigned char>(s[e])) || s[e] == '_'))
                ++e;
            const auto it = vars_.find(s.substr(i + 1, e - i - 1));
            if (it == vars_.end()) {
                ++i;
                continue;
            }
            s.replace(i, e - i, it->second);
            i += it->second.size();
        }
        std::filesystem::path p = fromUtf8(s);
        if (p.is_relative())
            p = dir_ / p;
        return p.lexically_normal();
    }

    std::vector<XprFile> filesOf(const XmlNode* set) const {
        std::vector<XprFile> out;
        for (const auto* f : set->all("File")) {
            XprFile x;
            x.pathText = f->attr("Path");
            x.path = expand(x.pathText);
            for (const auto& [k, v] : f->attrs)
                if (k != "Path")
                    x.attrs.emplace(k, v);
            if (const auto* info = f->first("FileInfo"))
                for (const auto* a : info->all("Attr"))
                    x.attrs.emplace(a->attr("Name"), a->attr("Val"));
            out.push_back(std::move(x));
        }
        return out;
    }

    static std::vector<std::string> splitList(const std::string& s) {
        std::vector<std::string> out;
        std::istringstream in(s);
        std::string item;
        while (in >> item)
            out.push_back(item);
        return out;
    }

    bool exists(const XprFile& f) {
        std::error_code ec;
        if (std::filesystem::exists(f.path, ec))
            return true;
        note("PRJ-XPR-MISSING", Severity::Warning, "file of the project not found: " + f.pathText,
             {"resolved to " + toUtf8(f.path)});
        return false;
    }

    void addIncludeDir(ProjectConfig& cfg, const std::filesystem::path& d) {
        if (std::ranges::find(cfg.includeDirs, d) == cfg.includeDirs.end())
            cfg.includeDirs.push_back(d);
    }

    void addSources(ProjectConfig& cfg, const std::vector<XprFile>& srcFiles, const std::vector<XprFile>& ipFiles) {
        static const std::set<std::string> hdl{".v", ".sv", ".vhd", ".vhdl"};
        static const std::set<std::string> headers{".vh", ".svh", ".h", ".vhh", ".inc"};
        static const std::set<std::string> ip{".xci", ".xcix", ".bd"};
        static const std::set<std::string> netlists{".edf", ".edif", ".edn", ".ngc", ".dcp"};
        // block designs (their IP is part of them) and core containers (their .xci is inside)
        std::vector<std::filesystem::path> bdDirs;
        std::set<std::filesystem::path> containers;
        for (const auto* list : {&srcFiles, &ipFiles})
            for (const auto& f : *list) {
                if (!f.enabled() || !f.usedIn("synthesis"))
                    continue;
                if (extensionOf(f.path) == ".bd")
                    bdDirs.push_back(f.path.parent_path());
                if (extensionOf(f.path) == ".xcix")
                    containers.insert(f.path.parent_path() / f.path.stem());
            }
        const auto inside = [](const std::filesystem::path& p, const std::filesystem::path& d) {
            const auto rel = p.lexically_relative(d);
            return !rel.empty() && *rel.begin() != "..";
        };
        std::set<std::filesystem::path> seen;
        std::size_t disabled = 0;
        const auto add = [&](const XprFile& f, bool fromIpSet) {
            const auto ext = extensionOf(f.path);
            if (!f.enabled()) {
                if (hdl.contains(ext) || ip.contains(ext))
                    ++disabled;
                return;
            }
            if (!f.usedIn("synthesis")) {
                skipped_.push_back(f.pathText + " (not used in synthesis)");
                return;
            }
            if (seen.contains(f.path))
                return;
            if (ext == ".xci" && (containers.contains(f.path.parent_path()) ||
                                  std::ranges::any_of(bdDirs, [&](const auto& d) { return inside(f.path, d); })))
                return; // the container or block design brings it
            if (fromIpSet && ext != ".xci" && ext != ".xcix")
                return; // the other files of an IP's own file set are its output products
            const bool globalInclude = f.one("IsGlobalInclude") == "1";
            if (headers.contains(ext) || globalInclude) {
                sv_ = sv_ || ext == ".svh";
                if (exists(f))
                    addIncludeDir(cfg, f.path.parent_path());
                seen.insert(f.path);
                return;
            }
            if (netlists.contains(ext)) {
                note("PRJ-XPR-NETLIST", Severity::Warning, "netlist of the project not analyzed: " + f.pathText,
                     {"OpenCDC reads HDL; a module defined only by this netlist stays unknown"});
                return;
            }
            if (!hdl.contains(ext) && !ip.contains(ext)) {
                skipped_.push_back(f.pathText);
                return;
            }
            seen.insert(f.path);
            if (!exists(f))
                return;
            cfg.sources.push_back(f.path);
            std::string lib;
            if (ext == ".vhd" || ext == ".vhdl") {
                lib = f.one("Library");
                if (lib.empty())
                    lib = defaultLib_;
            }
            cfg.sourceLibraries.push_back(lib);
        };
        for (const auto& f : srcFiles)
            add(f, false);
        for (const auto& f : ipFiles)
            add(f, true);
        if (disabled)
            note("PRJ-XPR-DISABLED", Severity::Info,
                 std::to_string(disabled) + " disabled source(s) of the project not read, as in Vivado", {});
    }

    void addConstraints(ProjectConfig& cfg, const std::vector<XprFile>& files) {
        for (const auto& f : files) {
            const auto ext = extensionOf(f.path);
            if (!f.enabled() || ext == ".dcp")
                continue;
            if (!f.usedIn("synthesis") && !f.usedIn("implementation"))
                continue;
            if (ext != ".xdc" && ext != ".sdc" && ext != ".tcl") {
                skipped_.push_back(f.pathText);
                continue;
            }
            const auto ref = f.one("ScopedToRef");
            if (ref.empty() && !f.one("ScopedToCell").empty()) {
                note("PRJ-XPR-SCOPED", Severity::Warning,
                     "constraint file scoped to cell '" + f.one("ScopedToCell") + "' only, not read: " + f.pathText,
                     {"OpenCDC scopes constraints to modules (SCOPED_TO_REF)"});
                continue;
            }
            if (!exists(f))
                continue;
            cfg.constraints.push_back(f.path);
            cfg.constraintScopes.push_back(ref);
        }
    }

    std::filesystem::path xpr_, dir_;
    std::string name_;
    std::string defaultLib_;
    bool sv_ = false;
    std::map<std::string, std::string> vars_;
    std::vector<Diagnostic> notes_;
    std::vector<std::string> skipped_;
};

} // namespace

bool isVivadoProjectFile(const std::filesystem::path& file) { return extensionOf(file) == ".xpr"; }

Result<ProjectConfig> projectFromXprText(std::string_view xprText, const std::filesystem::path& xprFile) {
    return Importer(xprFile).run(xprText);
}

Result<ProjectConfig> projectFromXprFile(const std::filesystem::path& xprFile) {
    std::ifstream in(xprFile, std::ios::binary);
    if (!in) {
        Diagnostic d{"PRJ-OPEN", Severity::Error, "cannot open Vivado project", {}, {}};
        d.location.file = toUtf8(xprFile);
        return d;
    }
    std::ostringstream text;
    text << in.rdbuf();
    return projectFromXprText(text.str(), xprFile);
}

} // namespace opencdc::engine
