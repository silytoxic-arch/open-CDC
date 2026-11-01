#include "engine/WaiverFile.h"

#include "core/PathUtil.h"
#include "engine/ProjectConfig.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <regex>
#include <sstream>

namespace opencdc::engine {

namespace {

std::uint32_t lineOfId(const std::string& text, const std::string& id) {
    const auto key = "\"" + id + "\"";
    for (std::size_t pos = text.find(key); pos != std::string::npos; pos = text.find(key, pos + 1)) {
        // Preceded by "id" : ...
        const auto before = text.rfind("\"id\"", pos);
        if (before != std::string::npos && text.find_first_not_of(" \t\r\n:", before + 4) == pos)
            return static_cast<std::uint32_t>(std::count(text.begin(), text.begin() + static_cast<long>(pos), '\n') + 1);
    }
    return 1;
}

} // namespace

void readWaiverFile(const std::filesystem::path& file, std::vector<cdc::WaiverRecord>& out, DiagnosticSink& sink) {
    const std::string name = toUtf8(file);
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        sink.report({"WAIVER-OPEN", Severity::Error, "cannot open waiver file '" + name + "'", {name, 0, 0}, {}});
        return;
    }
    std::stringstream buf;
    buf << in.rdbuf();
    const std::string text = buf.str();
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(text);
    }
    catch (const nlohmann::json::parse_error& e) {
        sink.report({"WAIVER-SCHEMA", Severity::Error, std::string("invalid JSON: ") + e.what(), {name, 1, 1}, {}});
        return;
    }
    auto schema = [&](const std::string& msg) {
        sink.report({"WAIVER-SCHEMA", Severity::Error, msg, {name, 1, 1}, {}});
    };
    if (!root.is_object() || root.value("format", "") != kWaiverFormatName)
        return schema(std::string("not an OpenCDC waiver file (format \"") + kWaiverFormatName + "\" expected)");
    if (root.value("version", 0) != kWaiverFormatVersion)
        return schema("unsupported waiver file version (1 expected)");
    const auto it = root.find("waivers");
    if (it == root.end() || !it->is_array())
        return schema("field 'waivers' must be an array");

    static const std::regex date(R"(\d{4}-\d{2}-\d{2})");
    for (const auto& w : *it) {
        if (!w.is_object()) {
            schema("every waiver must be an object");
            continue;
        }
        auto str = [&](const char* key) {
            auto f = w.find(key);
            return f != w.end() && f->is_string() ? f->get<std::string>() : std::string();
        };
        cdc::WaiverRecord r;
        r.id = str("id");
        r.rule = str("rule");
        r.fingerprint = str("fingerprint");
        r.source = str("source");
        r.destination = str("destination");
        r.reason = str("reason");
        r.author = str("author");
        r.expires = str("expires");
        r.loc = {name, r.id.empty() ? 1U : lineOfId(text, r.id), 1};
        auto invalid = [&](const std::string& why) {
            sink.report({"WAIVER-INVALID", Severity::Error,
                         "waiver '" + (r.id.empty() ? std::string("?") : r.id) + "' ignored: " + why, r.loc, {}});
        };
        for (const auto& [key, value] : w.items())
            if (key != "id" && key != "rule" && key != "fingerprint" && key != "source" && key != "destination" &&
                key != "reason" && key != "author" && key != "expires" && key != "comment")
                sink.report({"WAIVER-SCHEMA", Severity::Warning,
                             "waiver '" + r.id + "': unknown field '" + key + "' ignored", r.loc, {}});
        if (r.id.empty()) {
            invalid("missing 'id'");
            continue;
        }
        if (std::ranges::any_of(out, [&](const cdc::WaiverRecord& o) { return o.id == r.id; })) {
            invalid("duplicate id");
            continue;
        }
        if (r.reason.empty()) {
            invalid("a 'reason' is required");
            continue;
        }
        if (r.rule.empty() && r.fingerprint.empty() && r.source.empty() && r.destination.empty()) {
            invalid("it would cover every rule hit: give at least one of rule, fingerprint, source, destination");
            continue;
        }
        if (!r.expires.empty() && !std::regex_match(r.expires, date)) {
            invalid("'expires' must be YYYY-MM-DD");
            continue;
        }
        out.push_back(std::move(r));
    }
}

std::string waiverProblem(const cdc::WaiverRecord& w, const std::vector<std::string>& taken) {
    static const std::regex date(R"(\d{4}-\d{2}-\d{2})");
    if (w.id.empty())
        return "the waiver needs an id";
    if (std::ranges::find(taken, w.id) != taken.end())
        return "a waiver with id '" + w.id + "' already exists";
    if (w.reason.empty())
        return "a reason is required";
    if (w.rule.empty() && w.fingerprint.empty() && w.source.empty() && w.destination.empty())
        return "it would cover every rule hit: give at least one of rule, fingerprint, source, destination";
    if (!w.expires.empty() && !std::regex_match(w.expires, date))
        return "the expiry date must be YYYY-MM-DD";
    return {};
}

namespace {

/// Reads a waiver file for rewriting (field order kept); error text or "".
std::string loadForEdit(const std::filesystem::path& file, nlohmann::ordered_json& root) {
    const std::string name = toUtf8(file);
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return "cannot read '" + name + "'";
    std::stringstream buf;
    buf << in.rdbuf();
    try {
        root = nlohmann::ordered_json::parse(buf.str());
    }
    catch (const nlohmann::json::parse_error& e) {
        return "'" + name + "' is not valid JSON: " + e.what();
    }
    if (!root.is_object() || root.value("format", "") != kWaiverFormatName)
        return "'" + name + "' is not an OpenCDC waiver file";
    if (root.value("version", 0) != kWaiverFormatVersion)
        return "'" + name + "' has an unsupported waiver file version";
    if (!root.contains("waivers") || !root["waivers"].is_array())
        return "'" + name + "': field 'waivers' must be an array";
    return {};
}

/// Writes next to the file, then renames over it; error text or "".
std::string writeAtomically(const std::filesystem::path& file, const nlohmann::ordered_json& root) {
    auto tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return "cannot write '" + toUtf8(tmp) + "'";
        out << root.dump(2) << '\n';
        if (!out.flush())
            return "cannot write '" + toUtf8(tmp) + "'";
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        // Windows refuses to rename over a file another program holds open
        // (an editor showing it); copying over it still works then
        std::error_code copyEc, rmEc;
        std::filesystem::copy_file(tmp, file, std::filesystem::copy_options::overwrite_existing, copyEc);
        std::filesystem::remove(tmp, rmEc);
        if (copyEc)
            return "cannot replace '" + toUtf8(file) + "': " + ec.message();
    }
    return {};
}

/// Sets (or, when empty, removes) the selector and text fields of an entry;
/// the id and other fields ("comment") stay where they are.
void setFields(nlohmann::ordered_json& e, const cdc::WaiverRecord& w) {
    for (const auto& [key, value] : {std::pair<const char*, const std::string*>{"rule", &w.rule},
                                     {"fingerprint", &w.fingerprint},
                                     {"source", &w.source},
                                     {"destination", &w.destination},
                                     {"reason", &w.reason},
                                     {"author", &w.author},
                                     {"expires", &w.expires}}) {
        if (!value->empty())
            e[key] = *value;
        else
            e.erase(key);
    }
}

nlohmann::ordered_json* entryWithId(nlohmann::ordered_json& root, const std::string& id) {
    for (auto& e : root["waivers"])
        if (e.is_object() && e.value("id", "") == id)
            return &e;
    return nullptr;
}

} // namespace

std::string appendWaiver(const std::filesystem::path& file, const cdc::WaiverRecord& w,
                         const std::vector<std::string>& taken) {
    if (auto problem = waiverProblem(w, taken); !problem.empty())
        return problem;
    nlohmann::ordered_json root;
    std::error_code ec;
    if (std::filesystem::exists(file, ec)) {
        if (auto err = loadForEdit(file, root); !err.empty())
            return err;
        if (entryWithId(root, w.id))
            return "a waiver with id '" + w.id + "' already exists in '" + toUtf8(file) + "'";
    }
    else {
        root["format"] = kWaiverFormatName;
        root["version"] = kWaiverFormatVersion;
        root["waivers"] = nlohmann::ordered_json::array();
    }
    nlohmann::ordered_json e;
    e["id"] = w.id;
    setFields(e, w);
    root["waivers"].push_back(std::move(e));
    return writeAtomically(file, root);
}

std::string updateWaiver(const std::filesystem::path& file, const std::string& id, const cdc::WaiverRecord& w,
                         const std::vector<std::string>& taken) {
    std::vector<std::string> others = taken;
    std::erase(others, id); // keeping its own id is fine
    if (auto problem = waiverProblem(w, others); !problem.empty())
        return problem;
    nlohmann::ordered_json root;
    if (auto err = loadForEdit(file, root); !err.empty())
        return err;
    auto* e = entryWithId(root, id);
    if (!e)
        return "no waiver with id '" + id + "' in '" + toUtf8(file) + "'";
    if (w.id != id && entryWithId(root, w.id))
        return "a waiver with id '" + w.id + "' already exists in '" + toUtf8(file) + "'";
    (*e)["id"] = w.id;
    setFields(*e, w);
    return writeAtomically(file, root);
}

std::string removeWaiver(const std::filesystem::path& file, const std::string& id) {
    nlohmann::ordered_json root;
    if (auto err = loadForEdit(file, root); !err.empty())
        return err;
    auto& list = root["waivers"];
    for (auto it = list.begin(); it != list.end(); ++it)
        if (it->is_object() && it->value("id", "") == id) {
            list.erase(it);
            return writeAtomically(file, root);
        }
    return "no waiver with id '" + id + "' in '" + toUtf8(file) + "'";
}

std::string nextWaiverId(const std::vector<std::string>& taken) {
    for (std::size_t n = 1;; ++n) {
        std::string id = "W-" + std::to_string(n);
        if (std::ranges::find(taken, id) == taken.end())
            return id;
    }
}

cdc::WaiverRecord waiverFor(const cdc::CdcPath& path, const cdc::RuleHit& hit) {
    cdc::WaiverRecord w;
    w.rule = hit.ruleId;
    w.fingerprint = path.fingerprint;
    return w;
}

cdc::WaiverRecord waiverFor(const cdc::OutputCrossing& output) {
    cdc::WaiverRecord w;
    w.rule = "CDC-OUTPUT-CLOCK";
    w.fingerprint = output.fingerprint;
    return w;
}

AddedWaiver addProjectWaiver(ProjectConfig& cfg, cdc::WaiverRecord w, std::filesystem::path file) {
    AddedWaiver r;
    std::vector<std::string> taken;
    for (const auto& f : cfg.waivers) {
        std::error_code ec;
        if (!std::filesystem::exists(f, ec))
            continue;
        std::vector<cdc::WaiverRecord> records;
        CollectingDiagnosticSink ignored; // problems of the file are reported by the analysis
        readWaiverFile(f, records, ignored);
        for (auto& x : records)
            taken.push_back(std::move(x.id));
    }
    if (w.id.empty())
        w.id = nextWaiverId(taken);
    if (file.empty())
        file = !cfg.waivers.empty() ? cfg.waivers.front()
               : cfg.projectFile.empty() ? std::filesystem::absolute("waivers.json")
                                         : cfg.projectFile.parent_path() / "waivers.json";
    if (!file.is_absolute()) {
        std::error_code ec;
        file = std::filesystem::absolute(file, ec);
    }
    file = file.lexically_normal();
    r.error = appendWaiver(file, w, taken);
    if (!r.error.empty())
        return r;
    r.id = w.id;
    r.file = file;
    if (std::ranges::none_of(cfg.waivers, [&](const std::filesystem::path& p) { return p.lexically_normal() == file; })) {
        cfg.waivers.push_back(file);
        r.projectChanged = true;
    }
    return r;
}

} // namespace opencdc::engine
