#include "engine/QuartusProject.h"

#include "core/PathUtil.h"

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

std::string upper(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

std::string extensionOf(const std::filesystem::path& p) { return lower(toUtf8(p.extension())); }

/// A path with '/' separators, as Tcl writes them.
std::string generic(const std::filesystem::path& p) {
    auto s = toUtf8(p);
    std::ranges::replace(s, '\\', '/');
    return s;
}

std::optional<std::string> readText(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

/// The elements of a Tcl list (braces, quotes, backslashes).
std::vector<std::string> splitTclList(std::string_view s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
        if (i >= s.size())
            break;
        std::string item;
        if (s[i] == '{') {
            int depth = 1;
            ++i;
            while (i < s.size() && depth) {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    item += s[i++];
                }
                else if (s[i] == '{')
                    ++depth;
                else if (s[i] == '}' && --depth == 0)
                    break;
                item += s[i++];
            }
            ++i;
        }
        else if (s[i] == '"') {
            ++i;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size())
                    ++i;
                item += s[i++];
            }
            ++i;
        }
        else {
            while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) {
                if (s[i] == '\\' && i + 1 < s.size())
                    ++i;
                item += s[i++];
            }
        }
        out.push_back(std::move(item));
    }
    return out;
}

/// A word as a list element (braced when needed).
std::string tclQuote(const std::string& s) {
    if (s.empty())
        return "{}";
    if (std::ranges::any_of(s, [](char c) { return std::isspace(static_cast<unsigned char>(c)) || c == '{' || c == '}' ||
                                                   c == '"' || c == '[' || c == '$' || c == '\\' || c == ';'; }))
        return "{" + s + "}";
    return s;
}

/// The assignments Quartus settings scripts make, evaluated with the subset
/// of Tcl they use. Unknown commands give "" and are collected.
class QsfInterp {
public:
    using Handler = std::function<std::string(const std::vector<std::string>&)>;

    std::map<std::string, std::string> vars;
    std::set<std::string> unknownCommands;
    std::string error; ///< the first evaluation error, if any
    std::map<std::string, Handler> commands;

    std::string eval(std::string_view script) {
        std::string result;
        std::size_t i = 0;
        while (i < script.size() && error.empty()) {
            // skip separators and comments
            while (i < script.size() && (std::isspace(static_cast<unsigned char>(script[i])) || script[i] == ';'))
                ++i;
            if (i >= script.size())
                break;
            if (script[i] == '#') {
                while (i < script.size() && script[i] != '\n') {
                    if (script[i] == '\\' && i + 1 < script.size())
                        ++i;
                    ++i;
                }
                continue;
            }
            std::vector<std::string> words;
            if (!parseCommand(script, i, words))
                break;
            if (!words.empty())
                result = invoke(words);
        }
        return result;
    }

private:
    std::string invoke(const std::vector<std::string>& words) {
        if (const auto it = commands.find(words[0]); it != commands.end())
            return it->second(words);
        unknownCommands.insert(words[0]);
        return {};
    }

    /// One command's words, substituted; `i` ends after its terminator.
    bool parseCommand(std::string_view s, std::size_t& i, std::vector<std::string>& words) {
        while (i < s.size()) {
            // word separators within a command
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' ||
                                    (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '\n'))) {
                if (s[i] == '\\')
                    ++i;
                ++i;
            }
            if (i >= s.size() || s[i] == '\n' || s[i] == ';') {
                if (i < s.size())
                    ++i;
                return true;
            }
            std::string w;
            if (s[i] == '{') {
                int depth = 1;
                std::size_t j = i + 1;
                for (; j < s.size() && depth; ++j) {
                    if (s[j] == '\\')
                        ++j;
                    else if (s[j] == '{')
                        ++depth;
                    else if (s[j] == '}')
                        --depth;
                }
                if (depth) {
                    error = "unbalanced braces";
                    return false;
                }
                w = std::string(s.substr(i + 1, j - i - 2));
                i = j;
            }
            else if (s[i] == '"') {
                ++i;
                if (!substitute(s, i, w, '"'))
                    return false;
                ++i; // the closing quote
            }
            else if (!substitute(s, i, w, 0))
                return false;
            words.push_back(std::move(w));
        }
        return true;
    }

    /// Substitutes up to `end` ('"'), or up to a word end when `end` is 0.
    bool substitute(std::string_view s, std::size_t& i, std::string& out, char end) {
        while (i < s.size()) {
            const char c = s[i];
            if (end ? c == end : (std::isspace(static_cast<unsigned char>(c)) || c == ';'))
                return true;
            if (c == '\\' && i + 1 < s.size()) {
                const char n = s[i + 1];
                out += n == 'n' ? '\n' : n == 't' ? '\t' : n == '\n' ? ' ' : n;
                i += 2;
            }
            else if (c == '[') {
                int depth = 1;
                std::size_t j = i + 1;
                char quote = 0;
                for (; j < s.size() && depth; ++j) {
                    if (s[j] == '\\') {
                        ++j;
                        continue;
                    }
                    if (quote) {
                        if (s[j] == quote)
                            quote = 0;
                        continue;
                    }
                    if (s[j] == '{') {
                        int b = 1;
                        for (++j; j < s.size() && b; ++j)
                            b += s[j] == '{' ? 1 : s[j] == '}' ? -1 : 0;
                        --j;
                        continue;
                    }
                    if (s[j] == '[')
                        ++depth;
                    else if (s[j] == ']')
                        --depth;
                }
                if (depth) {
                    error = "unbalanced brackets";
                    return false;
                }
                const auto inner = s.substr(i + 1, j - i - 2);
                std::size_t k = 0;
                while (k < inner.size() && std::isspace(static_cast<unsigned char>(inner[k])))
                    ++k;
                std::size_t e = k;
                while (e < inner.size() && !std::isspace(static_cast<unsigned char>(inner[e])))
                    ++e;
                if (commands.contains(std::string(inner.substr(k, e - k))))
                    out += eval(inner);
                else
                    out += s.substr(i, j - i); // a bus index such as SDRAM_A[0], as Quartus reads it
                i = j;
            }
            else if (c == '$' && i + 1 < s.size()) {
                std::size_t j = i + 1;
                std::string name;
                if (s[j] == '{') {
                    const auto e = s.find('}', j);
                    if (e == std::string_view::npos) {
                        error = "unbalanced braces";
                        return false;
                    }
                    name = std::string(s.substr(j + 1, e - j - 1));
                    j = e + 1;
                }
                else {
                    while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_' ||
                                            (s[j] == ':' && j + 1 < s.size() && s[j + 1] == ':'))) {
                        if (s[j] == ':') {
                            name += "::";
                            j += 2;
                        }
                        else
                            name += s[j++];
                    }
                    if (j < s.size() && s[j] == '(') {
                        // array element: the key is substituted
                        ++j;
                        std::string key;
                        if (!substitute(s, j, key, ')'))
                            return false;
                        ++j;
                        name += "(" + key + ")";
                    }
                }
                if (name.empty()) {
                    out += '$';
                    ++i;
                    continue;
                }
                if (name.starts_with("::"))
                    name = name.substr(2);
                const auto it = vars.find(name);
                if (it == vars.end()) {
                    error = "no such variable: $" + name;
                    return false;
                }
                out += it->second;
                i = j;
            }
            else {
                out += c;
                ++i;
            }
        }
        if (end) {
            error = std::string("missing ") + end;
            return false;
        }
        return true;
    }
};

struct Assignment {
    std::string name;
    std::string value;
    std::string library;
    std::string entity;
    bool remove = false;
};

class Importer {
public:
    explicit Importer(const std::filesystem::path& qsf)
        : qsf_(std::filesystem::absolute(qsf).lexically_normal()), dir_(qsf_.parent_path()) {}

    std::string quartusVersion = "Version 18.1.0 Standard Edition";

    Result<ProjectConfig> run(std::string_view text) {
        cfg_.name = toUtf8(qsf_.stem());
        cfg_.importedFrom = qsf_;
        setupInterp();
        scripts_.push_back(qsf_);
        interp_.vars["quartus(qip_path)"] = generic(dir_) + "/";
        interp_.eval(text);
        if (!interp_.error.empty())
            return fail(toUtf8(qsf_.filename()) + ": " + interp_.error);
        if (cfg_.sources.empty() && cfg_.top.empty())
            return fail("not a Quartus settings file: no design files and no TOP_LEVEL_ENTITY");

        // SystemVerilog when Quartus reads any file as SystemVerilog
        if (systemVerilog_ || std::ranges::any_of(cfg_.sources, [](const auto& f) {
                const auto e = extensionOf(f);
                return e == ".sv" || e == ".svh";
            }))
            cfg_.language = frontend::LanguageStd::SystemVerilog2017;

        if (!skipped_.empty())
            note("PRJ-QSF-SKIPPED", Severity::Info,
                 std::to_string(skipped_.size()) + " file(s) of the project are not read by OpenCDC (memory "
                                                   "initialization and other files)",
                 skipped_);
        std::set<std::string> unknown = interp_.unknownCommands;
        for (const auto* ignored : {"package", "post_message", "puts", "project_open", "project_close", "export_assignments",
                                    "global", "variable", "load_package", "set_user_option", "execute_module"})
            unknown.erase(ignored);
        if (!unknown.empty()) {
            std::string list;
            for (const auto& u : unknown)
                list += (list.empty() ? "" : ", ") + u;
            note("PRJ-QSF-TCL", Severity::Info, "Tcl commands of the settings scripts not evaluated: " + list,
                 {"they gave an empty result; assignments they would make are missing"});
        }
        note("PRJ-QSF-IMPORT", Severity::Info,
             "Quartus project imported: " + std::to_string(cfg_.sources.size()) + " source(s), " +
                 std::to_string(cfg_.constraints.size()) + " constraint file(s), top " +
                 (cfg_.top.empty() ? std::string("not set") : "'" + cfg_.top + "'") + " (revision '" + cfg_.name +
                 "', " + std::to_string(qips_) + " IP file list(s))",
             {});
        cfg_.importNotes = std::move(notes_);
        return std::move(cfg_);
    }

private:
    Diagnostic fail(std::string message) const {
        Diagnostic d{"PRJ-QSF-PARSE", Severity::Error, std::move(message), {}, {}};
        d.location.file = toUtf8(qsf_);
        return d;
    }

    void note(std::string code, Severity sev, std::string message, std::vector<std::string> notes) {
        Diagnostic d{std::move(code), sev, std::move(message), {}, std::move(notes)};
        d.location.file = toUtf8(qsf_);
        notes_.push_back(std::move(d));
    }

    /// Paths in settings scripts and .qip files are relative to the project
    /// directory; a .qip's own files are usually given with $::quartus(qip_path).
    std::filesystem::path resolve(const std::string& text) const {
        std::filesystem::path p = fromUtf8(text);
        if (p.is_absolute())
            return p.lexically_normal();
        const auto inProject = (dir_ / p).lexically_normal();
        std::error_code ec;
        if (!std::filesystem::exists(inProject, ec) && scripts_.size() > 1) {
            const auto inScript = (scripts_.back().parent_path() / p).lexically_normal();
            if (std::filesystem::exists(inScript, ec))
                return inScript;
        }
        return inProject;
    }

    bool exists(const std::filesystem::path& p, const std::string& what) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec))
            return true;
        if (missing_.insert(p).second)
            note("PRJ-QSF-MISSING", Severity::Warning, what + " not found: " + toUtf8(p.lexically_relative(dir_)),
                 {"resolved to " + toUtf8(p)});
        return false;
    }

    void runScript(const std::filesystem::path& file, bool qip) {
        if (std::ranges::find(scripts_, file) != scripts_.end())
            return; // already being evaluated
        const auto text = readText(file);
        if (!text)
            return;
        const auto saved = interp_.vars["quartus(qip_path)"];
        if (qip) {
            ++qips_;
            // with a trailing '/', as Quartus sets it ([join [list $::quartus(qip_path) x.qip] {}])
            interp_.vars["quartus(qip_path)"] = generic(file.parent_path()) + "/";
        }
        scripts_.push_back(file);
        interp_.eval(*text);
        if (!interp_.error.empty()) {
            note("PRJ-QSF-TCL", Severity::Warning,
                 "evaluation of " + toUtf8(file.lexically_relative(dir_)) + " stopped: " + interp_.error,
                 {"the assignments after this point are missing"});
            interp_.error.clear();
        }
        scripts_.pop_back();
        interp_.vars["quartus(qip_path)"] = saved;
    }

    void addSource(const std::filesystem::path& p, const std::string& library) {
        if (!seen_.insert(p).second)
            return;
        if (!exists(p, "design file"))
            return;
        cfg_.sources.push_back(p);
        const auto e = extensionOf(p);
        cfg_.sourceLibraries.push_back(e == ".vhd" || e == ".vhdl" ? library : std::string());
    }

    void addIncludeDir(const std::filesystem::path& d) {
        if (std::ranges::find(cfg_.includeDirs, d) == cfg_.includeDirs.end())
            cfg_.includeDirs.push_back(d);
    }

    /// The .qip Quartus generates for a Platform Designer system or IP variation.
    void addGenerated(const std::filesystem::path& spec, const std::string& kind) {
        if (!exists(spec, kind))
            return;
        const auto stem = toUtf8(spec.stem());
        const auto d = spec.parent_path();
        for (const auto& qip : {d / stem / "synthesis" / (stem + ".qip"), d / stem / (stem + ".qip"),
                                d / (stem + ".qip")}) {
            std::error_code ec;
            if (std::filesystem::exists(qip, ec)) {
                runScript(qip.lexically_normal(), true);
                return;
            }
        }
        note("PRJ-QSF-NOT-GENERATED", Severity::Warning,
             kind + " " + toUtf8(spec.lexically_relative(dir_)) + " has no generated synthesis files",
             {"expected " + toUtf8((d / stem / "synthesis" / (stem + ".qip")).lexically_relative(dir_)),
              "generate it (qsys-generate " + toUtf8(spec.filename()) +
                  " --synthesis=VERILOG, or in Quartus) or add its HDL to the project"});
    }

    void assign(const Assignment& a) {
        if (a.remove)
            return;
        const auto n = upper(a.name);
        const auto& v = a.value;
        if (n == "VERILOG_FILE" || n == "SYSTEMVERILOG_FILE" || n == "VHDL_FILE") {
            if (n == "SYSTEMVERILOG_FILE")
                systemVerilog_ = true;
            addSource(resolve(v), a.library.empty() ? "work" : a.library);
        }
        else if (n == "QIP_FILE") {
            const auto p = resolve(v);
            if (exists(p, "IP file list"))
                runScript(p, true);
        }
        else if (n == "QSYS_FILE")
            addGenerated(resolve(v), "Platform Designer system");
        else if (n == "IP_FILE")
            addGenerated(resolve(v), "IP variation");
        else if (n == "SDC_FILE" || n == "SDC_ENTITY_FILE") {
            const auto p = resolve(v);
            if (seen_.insert(p).second && exists(p, "constraint file")) {
                cfg_.constraints.push_back(p);
                cfg_.constraintScopes.emplace_back();
            }
        }
        else if (n == "SEARCH_PATH" || n == "USER_LIBRARIES") {
            for (const auto& item : n == "USER_LIBRARIES" ? splitSemicolons(v) : std::vector<std::string>{v})
                addIncludeDir(resolve(item));
        }
        else if (n == "VERILOG_INCLUDE_FILE" || n == "SYSTEMVERILOG_INCLUDE_FILE") {
            const auto p = resolve(v);
            if (exists(p, "include file"))
                addIncludeDir(p.parent_path());
        }
        else if (n == "VERILOG_MACRO") {
            const auto eq = v.find('=');
            if (eq == std::string::npos)
                cfg_.defines.push_back({v, std::nullopt});
            else
                cfg_.defines.push_back({v.substr(0, eq), v.substr(eq + 1)});
        }
        else if (n == "TOP_LEVEL_ENTITY")
            cfg_.top = v;
        else if (n == "VERILOG_INPUT_VERSION") {
            if (upper(v).starts_with("SYSTEMVERILOG"))
                systemVerilog_ = true;
        }
        else if (n == "LAST_QUARTUS_VERSION")
            interp_.vars["quartus(version)"] = "Version " + v;
        else if (n == "AHDL_FILE" || n == "BDF_FILE" || n == "GDF_FILE" || n == "VQM_FILE" || n == "EDIF_FILE" ||
                 n == "QXP_FILE" || n == "TDF_FILE")
            note("PRJ-QSF-UNSUPPORTED-FILE", Severity::Warning,
                 n + " not read: " + v, {"OpenCDC reads Verilog, SystemVerilog and VHDL; a module defined only here "
                                         "stays unknown (convert it to HDL in Quartus)"});
        else if (n == "PRE_FLOW_SCRIPT_FILE")
            note("PRJ-QSF-PRE-FLOW", Severity::Info,
                 "the project runs a script before compilation (" + v + "), which may generate design files",
                 {"OpenCDC does not run it: compile the project in Quartus once, or run the script, before the "
                  "analysis"});
        else if (n == "MIF_FILE" || n == "HEX_FILE" || n == "TCL_SCRIPT_FILE" || n == "SIGNALTAP_FILE" ||
                 n == "SOURCE_FILE" || n == "SIP_FILE" || n == "SPD_FILE")
            skipped_.push_back(n + " " + v);
    }

    static std::vector<std::string> splitSemicolons(const std::string& s) {
        std::vector<std::string> out;
        std::string item;
        std::istringstream in(s);
        while (std::getline(in, item, ';'))
            if (!item.empty())
                out.push_back(item);
        return out;
    }

    /// set_global_assignment and friends: options and the value.
    static Assignment parseAssignment(const std::vector<std::string>& w) {
        Assignment a;
        std::vector<std::string> rest;
        for (std::size_t i = 1; i < w.size(); ++i) {
            const auto& x = w[i];
            const bool hasArg = x == "-name" || x == "-library" || x == "-entity" || x == "-section_id" ||
                                x == "-tag" || x == "-comment" || x == "-to" || x == "-from" || x == "-hdl_version" ||
                                x == "-rise" || x == "-fall";
            if (hasArg && i + 1 < w.size()) {
                if (x == "-name")
                    a.name = w[i + 1];
                else if (x == "-library")
                    a.library = w[i + 1];
                else if (x == "-entity")
                    a.entity = w[i + 1];
                ++i;
            }
            else if (x == "-remove")
                a.remove = true;
            else if (x == "-disable")
                a.remove = true;
            else
                rest.push_back(x);
        }
        if (!rest.empty())
            a.value = rest.back();
        return a;
    }

    void setupInterp() {
        auto& c = interp_.commands;
        interp_.vars["quartus(version)"] = quartusVersion;
        c["set"] = [this](const std::vector<std::string>& w) -> std::string {
            if (w.size() < 2)
                return {};
            auto name = w[1].starts_with("::") ? w[1].substr(2) : w[1];
            if (w.size() >= 3)
                interp_.vars[name] = w[2];
            const auto it = interp_.vars.find(name);
            return it == interp_.vars.end() ? std::string() : it->second;
        };
        c["set_global_assignment"] = [this](const std::vector<std::string>& w) {
            assign(parseAssignment(w));
            return std::string();
        };
        for (const auto* ignored : {"set_instance_assignment", "set_location_assignment", "set_io_assignment",
                                    "set_time_format", "set_global_assignment_tag"})
            c[ignored] = [](const std::vector<std::string>&) { return std::string(); };
        c["set_parameter"] = [this](const std::vector<std::string>& w) {
            const auto a = parseAssignment(w);
            const bool scoped = std::ranges::find(w, std::string("-to")) != w.end() || !a.entity.empty();
            if (!a.name.empty() && !scoped && !a.remove)
                cfg_.parameters.emplace_back(a.name, a.value);
            return std::string();
        };
        c["source"] = [this](const std::vector<std::string>& w) {
            if (w.size() >= 2) {
                const auto p = resolve(w.back());
                if (exists(p, "script"))
                    runScript(p, extensionOf(p) == ".qip");
            }
            return std::string();
        };
        c["file"] = [](const std::vector<std::string>& w) -> std::string {
            if (w.size() < 3)
                return {};
            const auto& sub = w[1];
            if (sub == "join") {
                std::filesystem::path p;
                for (std::size_t i = 2; i < w.size(); ++i) {
                    const std::filesystem::path part = fromUtf8(w[i]);
                    p = part.is_absolute() ? part : p / part;
                }
                return generic(p);
            }
            const std::filesystem::path p = fromUtf8(w[2]);
            if (sub == "dirname")
                return generic(p.parent_path());
            if (sub == "tail")
                return toUtf8(p.filename());
            if (sub == "rootname")
                return generic((p.parent_path() / p.stem()));
            if (sub == "extension")
                return toUtf8(p.extension());
            if (sub == "normalize")
                return generic(std::filesystem::absolute(p).lexically_normal());
            if (sub == "exists") {
                std::error_code ec;
                return std::filesystem::exists(p, ec) ? "1" : "0";
            }
            return {};
        };
        c["list"] = [](const std::vector<std::string>& w) {
            std::string out;
            for (std::size_t i = 1; i < w.size(); ++i)
                out += (i > 1 ? " " : "") + tclQuote(w[i]);
            return out;
        };
        c["concat"] = [](const std::vector<std::string>& w) {
            std::string out;
            for (std::size_t i = 1; i < w.size(); ++i)
                for (const auto& e : splitTclList(w[i]))
                    out += (out.empty() ? "" : " ") + tclQuote(e);
            return out;
        };
        c["join"] = [](const std::vector<std::string>& w) -> std::string {
            if (w.size() < 2)
                return {};
            const std::string sep = w.size() >= 3 ? w[2] : " ";
            std::string out;
            bool first = true;
            for (const auto& e : splitTclList(w[1])) {
                out += (first ? "" : sep) + e;
                first = false;
            }
            return out;
        };
        c["lindex"] = [](const std::vector<std::string>& w) -> std::string {
            if (w.size() < 3)
                return {};
            const auto l = splitTclList(w[1]);
            const auto idx = w[2] == "end" ? static_cast<long>(l.size()) - 1 : std::strtol(w[2].c_str(), nullptr, 10);
            return idx >= 0 && idx < static_cast<long>(l.size()) ? l[static_cast<std::size_t>(idx)] : std::string();
        };
        c["llength"] = [](const std::vector<std::string>& w) {
            return w.size() < 2 ? std::string("0") : std::to_string(splitTclList(w[1]).size());
        };
        c["regexp"] = [](const std::vector<std::string>& w) -> std::string {
            bool inl = false, nocase = false;
            std::size_t i = 1;
            for (; i < w.size() && w[i].starts_with("-"); ++i) {
                if (w[i] == "-inline")
                    inl = true;
                else if (w[i] == "-nocase")
                    nocase = true;
                else if (w[i] == "--") {
                    ++i;
                    break;
                }
            }
            if (i + 1 >= w.size())
                return {};
            try {
                const std::regex re(w[i], nocase ? std::regex::ECMAScript | std::regex::icase : std::regex::ECMAScript);
                std::smatch m;
                if (!std::regex_search(w[i + 1], m, re))
                    return inl ? std::string() : std::string("0");
                if (!inl)
                    return "1";
                std::string out;
                for (std::size_t k = 0; k < m.size(); ++k)
                    out += (k ? " " : "") + tclQuote(m[k].str());
                return out;
            }
            catch (const std::regex_error&) {
                return {};
            }
        };
        c["string"] = [](const std::vector<std::string>& w) -> std::string {
            if (w.size() < 3)
                return {};
            if (w[1] == "tolower")
                return lower(w[2]);
            if (w[1] == "toupper")
                return upper(w[2]);
            if (w[1] == "trim") {
                auto s = w[2];
                const auto b = s.find_first_not_of(" \t\r\n");
                const auto e = s.find_last_not_of(" \t\r\n");
                return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
            }
            if (w[1] == "equal" && w.size() >= 4)
                return w[2] == w[3] ? "1" : "0";
            if (w[1] == "length")
                return std::to_string(w[2].size());
            return {};
        };
    }

    std::filesystem::path qsf_, dir_;
    ProjectConfig cfg_;
    QsfInterp interp_;
    std::vector<std::filesystem::path> scripts_; ///< being evaluated, innermost last
    std::set<std::filesystem::path> seen_, missing_;
    std::vector<Diagnostic> notes_;
    std::vector<std::string> skipped_;
    std::size_t qips_ = 0;
    bool systemVerilog_ = false;
};

/// The assignments of a .qpf: name -> values.
std::multimap<std::string, std::string> qpfAssignments(const std::string& text) {
    std::multimap<std::string, std::string> out;
    static const std::regex line(R"re(^\s*([A-Z_]+)\s*=\s*"([^"]*)")re");
    std::istringstream in(text);
    std::string l;
    while (std::getline(in, l)) {
        std::smatch m;
        if (std::regex_search(l, m, line))
            out.emplace(m[1].str(), m[2].str());
    }
    return out;
}

} // namespace

bool isQuartusProjectFile(const std::filesystem::path& file) {
    const auto e = extensionOf(file);
    return e == ".qpf" || e == ".qsf";
}

Result<ProjectConfig> projectFromQsfText(std::string_view qsfText, const std::filesystem::path& qsfFile) {
    return Importer(qsfFile).run(qsfText);
}

Result<ProjectConfig> projectFromQuartusFile(const std::filesystem::path& file, const std::string& revision) {
    const auto open = [&](const std::filesystem::path& f) -> Diagnostic {
        Diagnostic d{"PRJ-OPEN", Severity::Error, "cannot open Quartus project file", {}, {}};
        d.location.file = toUtf8(f);
        return d;
    };
    auto qsf = file;
    std::string version;
    std::vector<std::string> otherRevisions;
    if (extensionOf(file) == ".qpf") {
        const auto text = readText(file);
        if (!text)
            return open(file);
        const auto a = qpfAssignments(*text);
        std::vector<std::string> revisions;
        for (auto [b, e] = a.equal_range("PROJECT_REVISION"); b != e; ++b)
            revisions.push_back(b->second);
        if (const auto v = a.find("QUARTUS_VERSION"); v != a.end())
            version = v->second;
        std::string rev = revision;
        if (rev.empty()) {
            if (revisions.empty()) {
                Diagnostic d{"PRJ-QSF-PARSE", Severity::Error, "the Quartus project names no revision (PROJECT_REVISION)",
                             {}, {}};
                d.location.file = toUtf8(file);
                return d;
            }
            rev = revisions.front();
        }
        for (const auto& r : revisions)
            if (r != rev)
                otherRevisions.push_back(r);
        qsf = file.parent_path() / (rev + ".qsf");
    }
    const auto text = readText(qsf);
    if (!text)
        return open(qsf);
    Importer imp(qsf);
    if (!version.empty())
        imp.quartusVersion = "Version " + version;
    auto r = imp.run(*text);
    if (r && !otherRevisions.empty()) {
        std::string list;
        for (const auto& o : otherRevisions)
            list += (list.empty() ? "" : ", ") + o;
        Diagnostic d{"PRJ-QSF-REVISION", Severity::Info,
                     "the project's first revision is analyzed; other revisions: " + list,
                     {}, {"open the revision's .qsf to analyze another one"}};
        d.location.file = toUtf8(file);
        r.value().importNotes.insert(r.value().importNotes.begin(), std::move(d));
    }
    return r;
}

} // namespace opencdc::engine
