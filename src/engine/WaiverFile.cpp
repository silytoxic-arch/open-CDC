#include "engine/WaiverFile.h"

#include "core/PathUtil.h"

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

} // namespace opencdc::engine
