#include "engine/DiamondProject.h"

#include "core/PathUtil.h"
#include "engine/Xml.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace opencdc::engine {

namespace {

std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string extensionOf(const std::filesystem::path& p) { return lower(toUtf8(p.extension())); }

std::optional<std::string> readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

class Importer {
public:
    explicit Importer(const std::filesystem::path& ldfFile)
        : ldf_(std::filesystem::absolute(ldfFile).lexically_normal()), dir_(ldf_.parent_path()) {}

    Result<ProjectConfig> run(std::string_view text) {
        const auto doc = parseXml(text);
        const XmlNode* project = doc ? doc->first("BaliProject") : nullptr;
        if (!project)
            return fail("not a Lattice Diamond project (.ldf): no <BaliProject> element");
        const auto impls = project->all("Implementation");
        if (impls.empty())
            return fail("the project has no implementation");
        const std::string wanted = project->attr("default_implementation");
        const XmlNode* impl = impls.front();
        for (const auto* i : impls)
            if (i->attr("title") == wanted)
                impl = i;

        ProjectConfig cfg;
        cfg.name = project->attr("title").empty() ? toUtf8(ldf_.stem()) : project->attr("title");
        cfg.importedFrom = ldf_;
        std::string markedTop;
        std::vector<std::string> skipped;
        std::size_t excluded = 0;
        bool sv = false;
        for (const auto* src : impl->all("Source")) {
            const std::string name = src->attr("name");
            const std::string type = src->attr("type");
            const std::string shortType = lower(src->attr("type_short"));
            const XmlNode* opts = src->first("Options");
            const bool design = shortType == "verilog" || shortType == "vhdl" || shortType == "systemverilog" ||
                                type == "Verilog" || type == "VHDL" || type == "SystemVerilog";
            const bool ipx = shortType == "ipx";
            const bool lpf = shortType == "lpf";
            const bool sdc = shortType == "sdc" || shortType == "fdc";
            if (lower(src->attr("excluded")) == "true") {
                if (design || ipx || lpf || sdc)
                    ++excluded;
                continue;
            }
            if (!design && !ipx && !lpf && !sdc) {
                skipped.push_back(name + " (" + (type.empty() ? shortType : type) + ")");
                continue;
            }
            const auto p = resolve(dir_, name);
            if (!exists(p)) {
                note("PRJ-LDF-MISSING", Severity::Warning, "file of the project not found: " + name,
                     {"resolved to " + toUtf8(p)});
                continue;
            }
            if (lpf || sdc) {
                cfg.constraints.push_back(p);
                continue;
            }
            if (ipx) {
                addIpx(cfg, p, name, sv);
                continue;
            }
            if (opts && !opts->attr("top_module").empty())
                markedTop = opts->attr("top_module");
            addSource(cfg, p, opts ? opts->attr("lib") : std::string(), sv);
        }
        if (cfg.sources.empty())
            return fail("implementation '" + impl->attr("title") + "' has no design files that exist");
        if (const auto* o = impl->first("Options")) {
            cfg.top = o->attr("top");
            if (cfg.top.empty())
                cfg.top = o->attr("def_top");
        }
        if (cfg.top.empty())
            cfg.top = markedTop;
        if (sv)
            cfg.language = frontend::LanguageStd::SystemVerilog2017;

        if (impls.size() > 1) {
            std::vector<std::string> others;
            for (const auto* i : impls)
                if (i != impl)
                    others.push_back(i->attr("title"));
            note("PRJ-LDF-IMPLEMENTATION", Severity::Info,
                 "implementation '" + impl->attr("title") + "' imported; " + std::to_string(others.size()) +
                     " other implementation(s) not read",
                 others);
        }
        if (excluded > 0)
            note("PRJ-LDF-SKIPPED", Severity::Info,
                 std::to_string(excluded) + " file(s) are excluded from the implementation", {});
        if (!skipped.empty())
            note("PRJ-LDF-SKIPPED", Severity::Info,
                 std::to_string(skipped.size()) + " file(s) of the project are not read by OpenCDC", skipped);
        const std::string device = project->attr("device");
        note("PRJ-LDF-IMPORT", Severity::Info,
             "Lattice Diamond project imported: " + std::to_string(cfg.sources.size()) + " source(s), " +
                 std::to_string(cfg.constraints.size()) + " constraint file(s), top " +
                 (cfg.top.empty() ? std::string("not set (found from the design)") : "'" + cfg.top + "'") +
                 " (implementation '" + impl->attr("title") + "'" +
                 (device.empty() ? std::string() : ", device " + device) + ")",
             {});
        cfg.importNotes = std::move(notes_);
        return cfg;
    }

private:
    static std::filesystem::path resolve(const std::filesystem::path& base, const std::string& name) {
        std::string n = name;
        std::ranges::replace(n, '\\', '/');
        std::filesystem::path p = fromUtf8(n);
        if (p.is_relative())
            p = base / p;
        return p.lexically_normal();
    }

    static bool exists(const std::filesystem::path& p) {
        std::error_code ec;
        return std::filesystem::is_regular_file(p, ec);
    }

    static void addSource(ProjectConfig& cfg, const std::filesystem::path& p, std::string lib, bool& sv) {
        if (std::ranges::find(cfg.sources, p) != cfg.sources.end())
            return;
        const auto ext = extensionOf(p);
        cfg.sources.push_back(p);
        cfg.sourceLibraries.push_back(ext == ".vhd" || ext == ".vhdl" ? std::move(lib) : std::string());
        if (ext == ".sv" || ext == ".svh")
            sv = true;
    }

    /// An IPexpress module: the HDL it generated (<File type="top_level_verilog"/"top_level_vhdl">).
    void addIpx(ProjectConfig& cfg, const std::filesystem::path& ipx, const std::string& name, bool& sv) {
        const auto text = readFile(ipx);
        const auto doc = text ? parseXml(*text) : nullptr;
        const XmlNode* mod = doc ? doc->first("DiamondModule") : nullptr;
        const XmlNode* pkg = mod ? mod->first("Package") : nullptr;
        if (!pkg) {
            note("PRJ-LDF-IPX", Severity::Warning, "IP module " + name + " could not be read (no <DiamondModule>)",
                 {"add the HDL that IPexpress generated for it to the sources"});
            return;
        }
        bool any = false, missing = false;
        for (const auto* f : pkg->all("File")) {
            const std::string type = f->attr("type");
            if (type != "top_level_verilog" && type != "top_level_vhdl")
                continue;
            const auto p = resolve(ipx.parent_path(), f->attr("name"));
            if (!exists(p)) {
                note("PRJ-LDF-IPX", Severity::Warning,
                     "IP module " + name + ": generated file not found: " + f->attr("name"),
                     {"generate the IP in Diamond (IPexpress) before the analysis"});
                missing = true;
                continue;
            }
            addSource(cfg, p, std::string(), sv);
            any = true;
        }
        if (!any && !missing)
            note("PRJ-LDF-IPX", Severity::Warning, "IP module " + name + " lists no generated HDL", {});
    }

    Diagnostic fail(std::string message) const {
        Diagnostic d{"PRJ-LDF-PARSE", Severity::Error, std::move(message), {}, {}};
        d.location.file = toUtf8(ldf_);
        return d;
    }

    void note(std::string code, Severity sev, std::string message, std::vector<std::string> notes) {
        Diagnostic d{std::move(code), sev, std::move(message), {}, std::move(notes)};
        d.location.file = toUtf8(ldf_);
        notes_.push_back(std::move(d));
    }

    std::filesystem::path ldf_;
    std::filesystem::path dir_;
    std::vector<Diagnostic> notes_;
};

} // namespace

bool isDiamondProjectFile(const std::filesystem::path& file) { return extensionOf(file) == ".ldf"; }

Result<ProjectConfig> projectFromLdfText(std::string_view text, const std::filesystem::path& ldfFile) {
    return Importer(ldfFile).run(text);
}

Result<ProjectConfig> projectFromLdfFile(const std::filesystem::path& file) {
    const auto text = readFile(file);
    if (!text) {
        Diagnostic d{"PRJ-OPEN", Severity::Error, "cannot open project file", {}, {}};
        d.location.file = toUtf8(file);
        return d;
    }
    return Importer(file).run(*text);
}

} // namespace opencdc::engine
