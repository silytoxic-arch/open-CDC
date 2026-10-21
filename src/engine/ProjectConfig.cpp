#include "engine/ProjectConfig.h"

#include "core/PathUtil.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace opencdc::engine {

namespace {

using nlohmann::json;

Diagnostic projectError(std::string code, std::string message, const std::filesystem::path& file = {}) {
    Diagnostic d{std::move(code), Severity::Error, std::move(message), {}, {}};
    if (!file.empty())
        d.location.file = toUtf8(file);
    return d;
}

std::filesystem::path resolve(const std::filesystem::path& baseDir, const std::string& p) {
    std::filesystem::path path = fromUtf8(p);
    if (path.is_relative())
        path = baseDir / path;
    return path.lexically_normal();
}

/// Reads an optional array of strings; returns an error message or empty string.
std::string readPathArray(const json& root, const char* key, const std::filesystem::path& baseDir,
                          std::vector<std::filesystem::path>& out, bool required) {
    auto it = root.find(key);
    if (it == root.end())
        return required ? std::string("missing required field '") + key + "'" : std::string();
    if (!it->is_array())
        return std::string("field '") + key + "' must be an array of strings";
    for (const auto& item : *it) {
        if (!item.is_string())
            return std::string("field '") + key + "' must contain only strings";
        out.push_back(resolve(baseDir, item.get<std::string>()));
    }
    return {};
}

} // namespace

std::string_view toString(UnknownClockPolicy p) noexcept {
    switch (p) {
        case UnknownClockPolicy::Report: return "report";
        case UnknownClockPolicy::AssumeAsync: return "assume_async";
        case UnknownClockPolicy::AssumeSync: return "assume_sync";
    }
    return "?";
}

frontend::FrontendInput ProjectConfig::toFrontendInput() const {
    frontend::FrontendInput in;
    in.sourceFiles = sources;
    in.includeDirs = includeDirs;
    in.defines = defines;
    in.top = top;
    in.language = language;
    in.sourceLibraries = sourceLibraries;
    in.topParameters = parameters;
    return in;
}

std::string ProjectConfig::languageSummary() const {
    const auto vhdl = std::ranges::count_if(sources, frontend::isVhdlFile);
    const std::string verilog(frontend::toString(language));
    if (vhdl == 0)
        return verilog;
    if (static_cast<std::size_t>(vhdl) == sources.size())
        return "vhdl";
    return verilog + " + vhdl";
}

Result<ProjectConfig> parseProjectJson(std::string_view jsonText, const std::filesystem::path& baseDir) {
    json root;
    try {
        root = json::parse(jsonText);
    }
    catch (const json::parse_error& e) {
        return projectError("PRJ-PARSE", std::string("invalid JSON: ") + e.what());
    }
    if (!root.is_object())
        return projectError("PRJ-SCHEMA", "project file must contain a JSON object");

    if (auto it = root.find("format"); it == root.end() || !it->is_string() ||
                                       it->get<std::string>() != kProjectFormatName) {
        return projectError("PRJ-SCHEMA", std::string("field 'format' must be \"") + kProjectFormatName + "\"");
    }
    if (auto it = root.find("version"); it == root.end() || !it->is_number_integer()) {
        return projectError("PRJ-SCHEMA", "field 'version' must be an integer");
    }
    else if (it->get<int>() != kProjectFormatVersion) {
        return projectError("PRJ-VERSION", "unsupported project format version " +
                                               std::to_string(it->get<int>()) + " (supported: " +
                                               std::to_string(kProjectFormatVersion) + ")");
    }

    ProjectConfig cfg;
    if (auto it = root.find("name"); it != root.end()) {
        if (!it->is_string())
            return projectError("PRJ-SCHEMA", "field 'name' must be a string");
        cfg.name = it->get<std::string>();
    }
    if (auto it = root.find("top"); it != root.end()) {
        if (!it->is_string())
            return projectError("PRJ-SCHEMA", "field 'top' must be a string");
        cfg.top = it->get<std::string>();
    }
    if (auto it = root.find("language"); it != root.end()) {
        if (!it->is_string())
            return projectError("PRJ-SCHEMA", "field 'language' must be a string");
        auto lang = frontend::parseLanguageStd(it->get<std::string>());
        if (!lang) {
            return projectError("PRJ-SCHEMA", "unknown language '" + it->get<std::string>() +
                                                  "' (expected verilog-2001, verilog-2005 or systemverilog-2017)");
        }
        cfg.language = *lang;
    }

    if (auto it = root.find("sources"); it == root.end()) {
        return projectError("PRJ-SCHEMA", "missing required field 'sources'");
    }
    else if (!it->is_array()) {
        return projectError("PRJ-SCHEMA", "field 'sources' must be an array");
    }
    else {
        // "file.v" or {"file": "x.vhd", "library": "olo"} (VHDL logical library)
        for (const auto& item : *it) {
            if (item.is_string()) {
                cfg.sources.push_back(resolve(baseDir, item.get<std::string>()));
                cfg.sourceLibraries.emplace_back();
                continue;
            }
            if (!item.is_object() || !item.contains("file") || !item["file"].is_string())
                return projectError("PRJ-SCHEMA",
                                    "entries of 'sources' must be strings or objects {\"file\": ..., \"library\": ...}");
            cfg.sources.push_back(resolve(baseDir, item["file"].get<std::string>()));
            std::string lib;
            if (auto l = item.find("library"); l != item.end()) {
                if (!l->is_string())
                    return projectError("PRJ-SCHEMA", "'library' of a source entry must be a string");
                lib = l->get<std::string>();
            }
            cfg.sourceLibraries.push_back(std::move(lib));
        }
    }
    if (cfg.sources.empty())
        return projectError("PRJ-SCHEMA", "field 'sources' must list at least one file");
    if (auto err = readPathArray(root, "includeDirs", baseDir, cfg.includeDirs, false); !err.empty())
        return projectError("PRJ-SCHEMA", err);
    if (auto err = readPathArray(root, "constraints", baseDir, cfg.constraints, false); !err.empty())
        return projectError("PRJ-SCHEMA", err);
    if (auto err = readPathArray(root, "waivers", baseDir, cfg.waivers, false); !err.empty())
        return projectError("PRJ-SCHEMA", err);

    if (auto it = root.find("defines"); it != root.end()) {
        if (!it->is_object())
            return projectError("PRJ-SCHEMA", "field 'defines' must be an object of name -> string|null");
        for (const auto& [name, value] : it->items()) {
            if (value.is_null())
                cfg.defines.push_back({name, std::nullopt});
            else if (value.is_string())
                cfg.defines.push_back({name, value.get<std::string>()});
            else if (value.is_number_integer())
                cfg.defines.push_back({name, std::to_string(value.get<long long>())});
            else
                return projectError("PRJ-SCHEMA", "define '" + name + "' must be a string, integer or null");
        }
    }

    if (auto it = root.find("parameters"); it != root.end()) {
        if (!it->is_object())
            return projectError("PRJ-SCHEMA", "field 'parameters' must be an object of name -> string|integer");
        for (const auto& [name, value] : it->items()) {
            if (value.is_string())
                cfg.parameters.emplace_back(name, value.get<std::string>());
            else if (value.is_number_integer())
                cfg.parameters.emplace_back(name, std::to_string(value.get<long long>()));
            else
                return projectError("PRJ-SCHEMA", "parameter '" + name + "' must be a string or an integer");
        }
    }

    if (auto it = root.find("options"); it != root.end()) {
        if (!it->is_object())
            return projectError("PRJ-SCHEMA", "field 'options' must be an object");
        for (const auto& [key, value] : it->items()) {
            if (key == "unknownClockRelation") {
                const std::string v = value.is_string() ? value.get<std::string>() : std::string();
                if (v == "report")
                    cfg.options.unknownClockRelation = UnknownClockPolicy::Report;
                else if (v == "assume_async")
                    cfg.options.unknownClockRelation = UnknownClockPolicy::AssumeAsync;
                else if (v == "assume_sync")
                    cfg.options.unknownClockRelation = UnknownClockPolicy::AssumeSync;
                else
                    return projectError("PRJ-SCHEMA",
                                        "options.unknownClockRelation must be report, assume_async or assume_sync");
            }
            else if (key == "minSyncStages") {
                if (!value.is_number_integer() || value.get<int>() < 1 || value.get<int>() > 8)
                    return projectError("PRJ-SCHEMA", "options.minSyncStages must be an integer between 1 and 8");
                cfg.options.minSyncStages = value.get<std::uint32_t>();
            }
            else if (key == "reconvergenceDepth") {
                if (!value.is_number_integer() || value.get<int>() < 0 || value.get<int>() > 8)
                    return projectError("PRJ-SCHEMA", "options.reconvergenceDepth must be an integer between 0 and 8");
                cfg.options.reconvergenceDepth = value.get<std::uint32_t>();
            }
            else {
                return projectError("PRJ-SCHEMA", "unknown option '" + key + "'");
            }
        }
    }

    if (cfg.name.empty())
        cfg.name = "unnamed";
    // No "language": SystemVerilog sources (.sv/.svh) select IEEE 1800-2017.
    if (root.find("language") == root.end()) {
        const bool sv = std::ranges::any_of(cfg.sources, [](const std::filesystem::path& f) {
            auto ext = f.extension().string();
            std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext == ".sv" || ext == ".svh";
        });
        if (sv)
            cfg.language = frontend::LanguageStd::SystemVerilog2017;
    }
    return cfg;
}

Result<ProjectConfig> loadProjectFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return projectError("PRJ-OPEN", "cannot open project file", file);
    std::ostringstream text;
    text << in.rdbuf();

    std::error_code ec;
    auto absFile = std::filesystem::absolute(file, ec).lexically_normal();
    auto result = parseProjectJson(text.str(), absFile.parent_path());
    if (!result) {
        Diagnostic d = std::move(result).error();
        d.location.file = toUtf8(absFile);
        return d;
    }
    ProjectConfig cfg = std::move(result).value();
    cfg.projectFile = absFile;
    return cfg;
}

} // namespace opencdc::engine
