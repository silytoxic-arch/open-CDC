#include "engine/GowinProject.h"

#include "core/PathUtil.h"
#include "engine/Xml.h"

#include <nlohmann/json.hpp>

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
    explicit Importer(const std::filesystem::path& gprjFile)
        : gprj_(std::filesystem::absolute(gprjFile).lexically_normal()), dir_(gprj_.parent_path()),
          name_(toUtf8(gprj_.stem())) {}

    Result<ProjectConfig> run(std::string_view text, std::string_view configJson) {
        const auto doc = parseXml(text);
        const XmlNode* project = doc ? doc->first("Project") : nullptr;
        if (!project)
            return fail("not a Gowin project (.gprj): no <Project> element");
        const XmlNode* list = project->first("FileList");
        if (!list)
            return fail("the project has no file list (no <FileList> element)");

        ProjectConfig cfg;
        cfg.name = name_;
        cfg.importedFrom = gprj_;
        std::string device;
        if (const auto* dev = project->first("Device"))
            device = dev->attr("pn").empty() ? dev->attr("name") : dev->attr("pn");

        std::vector<std::string> skipped;
        std::size_t disabled = 0;
        for (const auto* f : list->all("File")) {
            const std::string pathText = f->attr("path");
            const std::string type = f->attr("type");
            std::filesystem::path p = fromUtf8(pathText);
            if (p.is_relative())
                p = dir_ / p;
            p = p.lexically_normal();
            const bool design = type == "file.verilog" || type == "file.vhdl";
            const bool sdc = type == "file.sdc";
            if (f->attr("enable") == "0") {
                if (design || sdc)
                    ++disabled;
                continue;
            }
            if (!design && !sdc) {
                skipped.push_back(pathText + " (" + (type == "file.cst"   ? std::string("physical constraints")
                                                     : type == "file.gao" ? std::string("analyzer configuration")
                                                                          : type) +
                                  ")");
                continue;
            }
            std::error_code ec;
            if (!std::filesystem::exists(p, ec)) {
                note("PRJ-GPRJ-MISSING", Severity::Warning, "file of the project not found: " + pathText,
                     {"resolved to " + toUtf8(p)});
                continue;
            }
            if (sdc) {
                cfg.constraints.push_back(p);
                continue;
            }
            cfg.sources.push_back(p);
            cfg.sourceLibraries.emplace_back(type == "file.vhdl" ? f->attr("library") : std::string());
            if (extensionOf(p) == ".sv")
                sv_ = true;
        }
        if (cfg.sources.empty())
            return fail("the project has no design files of its own that exist (file.verilog / file.vhdl)");

        cfg.options.deepestRootTop = true; // until the settings name a top module
        cfg.options.gowinPrimitives = true;
        settings(cfg, configJson);
        if (sv_ && cfg.language != frontend::LanguageStd::SystemVerilog2017) {
            // .sv files are SystemVerilog whatever the project's Verilog setting
            cfg.language = frontend::LanguageStd::SystemVerilog2017;
        }

        if (disabled > 0)
            note("PRJ-GPRJ-SKIPPED", Severity::Info,
                 std::to_string(disabled) + " design or constraint file(s) are disabled in the project", {});
        if (!skipped.empty())
            note("PRJ-GPRJ-SKIPPED", Severity::Info,
                 std::to_string(skipped.size()) + " file(s) of the project are not read by OpenCDC", skipped);
        note("PRJ-GPRJ-IMPORT", Severity::Info,
             "Gowin project imported: " + std::to_string(cfg.sources.size()) + " source(s), " +
                 std::to_string(cfg.constraints.size()) + " constraint file(s), top " +
                 (cfg.top.empty() ? std::string("not set (found from the design)") : "'" + cfg.top + "'") +
                 (device.empty() ? std::string() : ", device " + device) +
                 (configFile_.empty() ? std::string(", no synthesis settings file") : ", settings " + configFile_),
             {});
        cfg.importNotes = std::move(notes_);
        return cfg;
    }

    void setConfigFile(std::string name) { configFile_ = std::move(name); }

private:
    /// TopModule, Verilog_Standard, IncludePath of the impl settings.
    void settings(ProjectConfig& cfg, std::string_view configJson) {
        if (configJson.empty())
            return;
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(configJson);
        }
        catch (const nlohmann::json::exception& e) {
            note("PRJ-GPRJ-CONFIG", Severity::Warning, "the synthesis settings could not be read: " + std::string(e.what()),
                 {configFile_});
            return;
        }
        if (!j.is_object())
            return;
        if (auto it = j.find("TopModule"); it != j.end() && it->is_string())
            cfg.top = it->get<std::string>();
        // no top module: Gowin EDA chooses the top itself; OpenCDC takes the root module
        // with the deepest hierarchy
        cfg.options.deepestRootTop = cfg.top.empty();
        if (auto it = j.find("Verilog_Standard"); it != j.end() && it->is_string()) {
            const auto v = it->get<std::string>();
            if (v == "Vlg_Std_Sysv2017") {
                // Gowin: the setting applies to every Verilog file, .v included
                cfg.language = frontend::LanguageStd::SystemVerilog2017;
                cfg.verilogFilesSystemVerilog = true;
            }
            else if (v == "Vlg_Std_V2001" || v == "Vlg_Std_V1995")
                cfg.language = frontend::LanguageStd::Verilog2001;
            else
                note("PRJ-GPRJ-CONFIG", Severity::Info, "Verilog standard '" + v + "' not known: Verilog-2001 is used",
                     {configFile_});
        }
        if (auto it = j.find("IncludePath"); it != j.end() && it->is_array())
            for (const auto& d : *it)
                if (d.is_string() && !d.get<std::string>().empty()) {
                    std::filesystem::path p = fromUtf8(d.get<std::string>());
                    if (p.is_relative())
                        p = dir_ / p;
                    p = p.lexically_normal();
                    if (std::ranges::find(cfg.includeDirs, p) == cfg.includeDirs.end())
                        cfg.includeDirs.push_back(p);
                }
    }

    Diagnostic fail(std::string message) const {
        Diagnostic d{"PRJ-GPRJ-PARSE", Severity::Error, std::move(message), {}, {}};
        d.location.file = toUtf8(gprj_);
        return d;
    }

    void note(std::string code, Severity sev, std::string message, std::vector<std::string> notes) {
        Diagnostic d{std::move(code), sev, std::move(message), {}, std::move(notes)};
        d.location.file = toUtf8(gprj_);
        notes_.push_back(std::move(d));
    }

    std::filesystem::path gprj_;
    std::filesystem::path dir_;
    std::string name_;
    std::string configFile_;
    bool sv_ = false;
    std::vector<Diagnostic> notes_;
};

} // namespace

bool isGowinProjectFile(const std::filesystem::path& file) { return extensionOf(file) == ".gprj"; }

Result<ProjectConfig> projectFromGprjText(std::string_view gprjText, std::string_view configJson,
                                          const std::filesystem::path& gprjFile) {
    Importer imp(gprjFile);
    return imp.run(gprjText, configJson);
}

Result<ProjectConfig> projectFromGprjFile(const std::filesystem::path& file) {
    const auto text = readFile(file);
    if (!text) {
        Diagnostic d{"PRJ-OPEN", Severity::Error, "cannot open project file", {}, {}};
        d.location.file = toUtf8(file);
        return d;
    }
    // the settings: impl/<project>_process_config.json, else impl/project_process_config.json
    const auto abs = std::filesystem::absolute(file).lexically_normal();
    const auto impl = abs.parent_path() / "impl";
    std::string configJson, configName;
    for (const auto& candidate : {impl / (toUtf8(abs.stem()) + "_process_config.json"), impl / "project_process_config.json"})
        if (auto t = readFile(candidate)) {
            configJson = std::move(*t);
            configName = candidate.lexically_relative(abs.parent_path()).generic_string();
            break;
        }
    Importer imp(file);
    imp.setConfigFile(configName);
    return imp.run(*text, configJson);
}

} // namespace opencdc::engine
