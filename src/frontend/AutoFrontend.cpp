// Default frontend: chooses the language frontend by source file extension
// (.vhd / .vhdl: VHDL, everything else: Verilog / SystemVerilog via slang)
// and links mixed-language designs.
//
// Mixed designs are elaborated in rounds. Each round compiles the Verilog
// sources plus a generated wrapper module that instantiates every Verilog
// module (with its parameter values) requested by VHDL code; instances of
// VHDL entities inside Verilog are kept as unresolved children with their
// parameter values and port actuals. The VHDL elaborator then adds its
// bodies to the same design, binding Verilog-requested entities and taking
// the bodies of requested Verilog modules from the wrapper. A request that
// was not in the wrapper yet triggers another round (fixpoint).

#include "core/PathUtil.h"
#include "core/TextEncoding.h"
#include "frontend/Frontend.h"
#include "frontend/MixedSupport.h"
#include "frontend/Primitives.h"
#include "frontend/BlockDesign.h"
#include "frontend/XciModels.h"
#include "core/Version.h"
#include "frontend/vhdl/VhdlElab.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <regex>
#include <sstream>
#include <map>
#include <set>
#include <tuple>

namespace opencdc::frontend {

bool isVhdlFile(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".vhd" || ext == ".vhdl" || ext == ".vho";
}

namespace {

constexpr const char* kWrapper = "opencdc_mixed_wrapper";
constexpr int kMaxRounds = 16;

std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// slang reports every unconnected port of an instance on its own; generated
/// code (LiteX, vendor wrappers) leaves dozens open. One note per instance:
/// FE-UNCONNECTED-PORTS listing the inputs and outputs.
void mergeUnconnectedPorts(std::vector<Diagnostic>& diags) {
    static const std::regex nameRe("port '([^']+)'");
    std::vector<Diagnostic> out;
    std::map<std::tuple<std::string, std::uint32_t, std::uint32_t>, std::size_t> site;
    std::map<std::size_t, std::pair<std::vector<std::string>, std::vector<std::string>>> ports; // inputs, outputs
    for (auto& d : diags) {
        const bool in = d.code == "FE-SLANG-UnconnectedInputPort";
        const bool outp = d.code == "FE-SLANG-UnconnectedOutputPort";
        std::smatch m;
        if ((!in && !outp) || !std::regex_search(d.message, m, nameRe)) {
            out.push_back(std::move(d));
            continue;
        }
        const auto key = std::make_tuple(d.location.file, d.location.line, d.location.column);
        auto [it, added] = site.try_emplace(key, out.size());
        if (added) {
            Diagnostic g = d;
            g.code = "FE-UNCONNECTED-PORTS";
            out.push_back(std::move(g));
        }
        (in ? ports[it->second].first : ports[it->second].second).push_back(m[1].str());
    }
    auto list = [](const std::vector<std::string>& v) {
        std::string s;
        for (std::size_t k = 0; k < v.size() && k < 8; ++k)
            s += (k ? ", " : "") + v[k];
        if (v.size() > 8)
            s += ", ... (" + std::to_string(v.size() - 8) + " more)";
        return s;
    };
    for (auto& [idx, io] : ports) {
        const auto& [ins, outs] = io;
        std::string msg = std::to_string(ins.size() + outs.size()) + " port(s) of this instance have no connection";
        if (!ins.empty())
            msg += "; inputs: " + list(ins);
        if (!outs.empty())
            msg += "; outputs: " + list(outs);
        out[idx].message = msg;
        // open outputs are common practice; an open input floats
        out[idx].severity = ins.empty() ? Severity::Info : Severity::Warning;
    }
    diags = std::move(out);
}

/// Verilog text of a parameter value.
std::string verilogValue(const ir::InstanceParam& p) {
    if (p.str) {
        std::string out = "\"";
        for (char c : *p.str) {
            if (c == '"' || c == '\\')
                out += '\\';
            out += c;
        }
        return out + "\"";
    }
    if (p.real) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.17g", *p.real);
        std::string s = buf;
        if (s.find_first_of(".eEn") == std::string::npos)
            s += ".0";
        return s;
    }
    if (p.bits) {
        if (p.isSigned) {
            if (auto v = p.bits->toInt())
                return *v < 0 ? "(" + std::to_string(*v) + ")" : std::to_string(*v);
        }
        return p.bits->toString();
    }
    return "0";
}

/// Directory with the vendor primitive models as files (stable paths, so
/// source locations of primitive registers stay valid after the run).
std::filesystem::path primitiveDir() {
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec) / ("opencdc-primitives-" + std::string(kVersionString));
    std::filesystem::create_directories(dir, ec);
    return dir;
}

/// Writes `text` to `path` unless it already has exactly that content.
void writeIfChanged(const std::filesystem::path& path, const std::string& text) {
    {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            if (ss.str() == text)
                return;
        }
    }
    // unique across processes: parallel analyses write the same models
    auto tmp = path;
    tmp += ".tmp" + uniqueToken();
    {
        std::ofstream out(tmp, std::ios::binary);
        out << text;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec)
        std::filesystem::remove(tmp, ec);
}

/// VHDL text without "--" comments (library clauses are looked for in it).
std::string withoutVhdlComments(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool inString = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\n')
            inString = false;
        if (c == '"')
            inString = !inString;
        if (!inString && c == '-' && i + 1 < text.size() && text[i + 1] == '-') {
            while (i < text.size() && text[i] != '\n')
                ++i;
            if (i < text.size())
                out += '\n';
            continue;
        }
        out += c;
    }
    return out;
}

struct Request {
    std::string module;
    std::vector<ir::InstanceParam> params;
    std::string text; ///< identity: module and parameter values
};

/// Keeps the diagnostics of one round; only the last round is reported.
class RoundSink final : public DiagnosticSink {
public:
    void report(Diagnostic d) override { diags.push_back(std::move(d)); }
    std::vector<Diagnostic> diags;
};

InstanceInfo makeInfo(const ir::Design& d, ir::InstanceId id) {
    const auto& n = d.instance(id);
    InstanceInfo info;
    info.name = n.name;
    info.definition = d.body(n.body).name;
    info.hierPath = n.hierPath;
    if (n.parent.valid()) {
        const auto& pb = d.body(d.instance(n.parent).body);
        if (n.childIndex < pb.children.size())
            info.location = pb.children[n.childIndex].loc;
    }
    else {
        info.location = d.body(n.body).loc;
    }
    for (auto c : n.children)
        info.children.push_back(makeInfo(d, c));
    return info;
}

/// New design with only the bodies reachable from `tops` (wrapper and stale
/// rounds dropped), and the instance trees of the tops.
std::shared_ptr<ir::Design> compact(const ir::Design& d, const std::vector<ir::ModuleId>& tops) {
    auto out = std::make_shared<ir::Design>();
    std::map<std::size_t, ir::ModuleId> remap;
    std::vector<std::size_t> work;
    auto visit = [&](ir::ModuleId id) {
        if (!id.valid() || remap.contains(id.index()))
            return;
        remap.emplace(id.index(), ir::ModuleId{});
        work.push_back(id.index());
    };
    for (auto t : tops)
        visit(t);
    std::vector<std::size_t> order;
    while (!work.empty()) {
        const std::size_t b = work.back();
        work.pop_back();
        order.push_back(b);
        for (const auto& c : d.bodies[b].children)
            visit(c.body);
    }
    std::ranges::sort(order); // keep the original (deterministic) body order
    for (std::size_t b : order)
        remap[b] = out->addBody(d.bodies[b]);
    for (auto& body : out->bodies)
        for (auto& c : body.children)
            if (c.body.valid())
                c.body = remap.at(c.body.index());
    std::set<std::string> kept;
    for (const auto& b : out->bodies)
        kept.insert(b.name);
    for (const auto& u : d.unsupported)
        if (kept.contains(u.module))
            out->unsupported.push_back(u);
    for (auto t : tops)
        if (t.valid())
            out->addTop(remap.at(t.index()));
    return out;
}

class MixedLinker {
public:
    MixedLinker(const FrontendInput& input, DiagnosticSink& sink) : input_(input), sink_(sink) {}

    Result<FrontendOutput> run() {
        FrontendOutput output;
        // ---- split the sources -------------------------------------------
        FrontendInput vin = input_;
        vin.sourceFiles.clear();
        vin.sourceLibraries.clear();
        vin.topParameters.clear();
        std::vector<std::filesystem::path> vhdlPaths;
        std::vector<std::string> vhdlLibs;
        for (std::size_t i = 0; i < input_.sourceFiles.size(); ++i) {
            const auto& f = input_.sourceFiles[i];
            if (isVhdlFile(f)) {
                vhdlPaths.push_back(f);
                vhdlLibs.push_back(i < input_.sourceLibraries.size() ? input_.sourceLibraries[i] : std::string());
            }
            else {
                vin.sourceFiles.push_back(f);
            }
        }
        // vendor component packages (unisim, xpm, altera_mf, Lattice, Gowin) for libraries the
        // VHDL code uses but the project does not provide
        {
            std::set<std::string> given;
            for (const auto& l : vhdlLibs)
                given.insert(lower(l.empty() ? std::string("work") : l));
            std::set<std::string> used;
            static const std::regex libRe(R"((^|[^A-Za-z0-9_])library\s+([^;]+);)", std::regex::icase);
            for (const auto& f : vhdlPaths) {
                std::ifstream in(f, std::ios::binary);
                std::ostringstream ss;
                ss << in.rdbuf();
                const std::string text = lower(withoutVhdlComments(ss.str()));
                for (auto it = std::sregex_iterator(text.begin(), text.end(), libRe); it != std::sregex_iterator(); ++it) {
                    std::stringstream names((*it)[2].str());
                    std::string n;
                    while (std::getline(names, n, ',')) {
                        n.erase(0, n.find_first_not_of(" \t\r\n"));
                        n.erase(n.find_last_not_of(" \t\r\n") + 1);
                        used.insert(n);
                    }
                }
            }
            for (const auto& pkg : PrimitiveLibrary::instance().vhdlPackages()) {
                if (!used.contains(pkg.library) || given.contains(pkg.library))
                    continue;
                const auto path = primitiveDir() / pkg.file;
                writeIfChanged(path, pkg.text);
                vhdlPaths.push_back(path);
                vhdlLibs.push_back(pkg.library);
                vendorVhdl_.push_back(path);
            }
        }
        RoundSink parseSink;
        auto ok = vhdl::loadSources(vhdlPaths, vhdlLibs, parseSink, files_);
        if (!ok.ok())
            return ok.error();
        for (auto& d : parseSink.diags)
            sink_.report(std::move(d));
        if (!ok.value())
            return output;

        // VHDL entity names (for slang: definitions provided elsewhere)
        std::set<std::string> vhdlEntities;
        std::vector<DefinitionInfo> vhdlDefs;
        {
            RoundSink scratch;
            vhdl::ElabOptions o;
            vhdl::Elaborator e(files_, scratch, o);
            vhdlDefs = e.definitions();
            for (const auto& d : vhdlDefs)
                vhdlEntities.insert(lower(d.name));
        }

        // wrapper file (rewritten every round)
        // a directory of its own: parallel analyses must not share it
        tmpDir_ = createUniqueTempDir("opencdc_mixed_");
        std::error_code ec;
        if (tmpDir_.empty()) {
            cleanup();
            return Diagnostic{"FE-MIXED-TEMP", Severity::Error,
                              "cannot create a temporary directory for the mixed-language wrapper", {}, {}};
        }
        const auto wrapperPath = tmpDir_ / "opencdc_mixed_wrapper.v";
        const auto userVerilog = vin.sourceFiles;
        const std::string wrapperUtf8 = toUtf8(std::filesystem::weakly_canonical(wrapperPath, ec));
        const std::string primDirUtf8 = toUtf8(std::filesystem::weakly_canonical(primitiveDir(), ec));
        const auto& prims = PrimitiveLibrary::instance();
        std::set<std::string> foreign = vhdlEntities;
        for (const auto& n : prims.names())
            foreign.insert(lower(n));

        std::vector<Diagnostic> lastDiags;
        std::shared_ptr<ir::Design> finalDesign;
        std::shared_ptr<const ir::Design> lastDesign; ///< last converged round, also with errors
        bool converged = false;
        for (int round = 0; round < kMaxRounds && !converged; ++round) {
            const std::string wrapperUsed = wrapperText();
            writeWrapper(wrapperPath);
            vin.sourceFiles = userVerilog;
            vin.sourceFiles.push_back(wrapperPath);
            std::string modelSignature;
            for (const auto& m : models_) {
                const std::string text = modelText(prims, m);
                // generator models depend on the parameters in use: one file per variant
                const auto path = primitiveDir() / (m + "_" + std::to_string(std::hash<std::string>{}(text) % 100000) + ".v");
                writeIfChanged(path, text);
                vin.sourceFiles.push_back(path);
                modelSignature += text;
            }
            RoundSink rs;
            detail::VerilogMixedOptions mo;
            mo.foreignModules = foreign;
            mo.lenientModules = models_;
            mo.blackBoxUnknown = input_.blackBoxUnknownModules;
            std::map<std::string, std::vector<std::string>> params;
            mo.moduleParams = &params;
            const bool verilogTop = !input_.top.empty() && !vhdlEntities.contains(lower(input_.top));
            if (verilogTop) {
                mo.tops = {input_.top, kWrapper};
                vin.topParameters = input_.topParameters;
            }
            else if (!input_.top.empty()) {
                mo.tops = {kWrapper};
            }
            auto vout = detail::elaborateVerilog(vin, rs, &mo);
            if (!vout.ok()) {
                cleanup();
                return vout.error();
            }
            for (auto& [m, ps] : params)
                verilogParams_[m] = ps;
            if (round == 0) {
                for (const auto& d : vout.value().summary.definitions)
                    if (d.name != kWrapper) {
                        verilogDefs_.push_back(d);
                        verilogModules_[lower(d.name)] = d.name;
                    }
                // modelled vendor primitives the user does not define (VHDL entities win for VHDL)
                // Verilog is case-sensitive: only a module named exactly like the
                // primitive replaces it (verilog-ethernet's 'iddr' wraps Xilinx 'IDDR')
                for (const auto& n : prims.names())
                    if (std::ranges::none_of(verilogDefs_, [&](const auto& d) { return d.name == n; }) &&
                        !vhdlEntities.contains(lower(n)))
                        primitiveModules_[lower(n)] = n;
                for (const auto& f : vout.value().summary.parsedFiles)
                    if (toUtf8(f) != wrapperUtf8)
                        parsedVerilog_.push_back(f);
                for (const auto& [key, name] : verilogModules_)
                    if (vhdlEntities.contains(key))
                        sink_.report({"FE-MIXED-DUPLICATE", Severity::Warning,
                                      "'" + name + "' is defined in Verilog and in VHDL: Verilog instances use the "
                                      "Verilog module, VHDL instances the VHDL entity",
                                      {}, {}});
            }
            if (!vout.value().design) {
                lastDiags = std::move(rs.diags);
                finalDesign.reset();
                break;
            }
            auto design = std::const_pointer_cast<ir::Design>(vout.value().design);
            // bodies of the requests in the wrapper
            resolved_.assign(requests_.size(), ir::ModuleId{});
            for (const auto& b : design->bodies) {
                if (b.name != kWrapper)
                    continue;
                for (const auto& c : b.children) {
                    if (c.name.starts_with("r") && c.name.size() > 1) {
                        const auto k = static_cast<std::size_t>(std::stoul(c.name.substr(1)));
                        if (k < resolved_.size())
                            resolved_[k] = c.body;
                    }
                }
            }
            const std::size_t knownRequests = requests_.size();

            // VHDL elaborator adding to the same design
            vhdl::ElabOptions o;
            o.top = verilogTop ? std::string() : input_.top;
            o.generics = verilogTop ? std::vector<std::pair<std::string, std::string>>{} : input_.topParameters;
            o.design = design;
            o.foreignModules = verilogModules_;
            for (const auto& [k, n] : primitiveModules_) {
                o.foreignModules.emplace(k, n);
                o.lenientModules.insert(k);
            }
            o.resolveForeign = [this](const std::string& module, const std::vector<ir::InstanceParam>& p) {
                return resolve(module, p);
            };
            // entities instantiated from Verilog are not tops
            const std::size_t slangBodies = design->bodies.size();
            for (std::size_t b = 0; b < slangBodies; ++b)
                for (const auto& c : design->bodies[b].children)
                    if (!c.body.valid() && vhdlEntities.contains(lower(c.definition)))
                        o.excludeTops.insert(lower(c.definition));
            vhdl::Elaborator elab(files_, rs, o);

            // Verilog -> VHDL
            for (std::size_t b = 0; b < slangBodies; ++b) {
                for (std::size_t ci = 0; ci < design->bodies[b].children.size(); ++ci) {
                    const ir::ChildInstance c = design->bodies[b].children[ci];
                    if (c.body.valid() || !vhdlEntities.contains(lower(c.definition)))
                        continue;
                    const ir::ModuleId id = elab.foreignEntity(c.definition, c.unresolvedParams, c.loc);
                    if (!id.valid())
                        continue;
                    bind(*design, b, ci, id, elab.foreignPorts(id), rs);
                }
            }
            // vendor primitives needed by Verilog instances and by VHDL requests
            for (std::size_t b = 0; b < slangBodies; ++b)
                for (const auto& c : design->bodies[b].children)
                    if (!c.body.valid())
                        if (auto it = primitiveModules_.find(lower(c.definition)); it != primitiveModules_.end()) {
                            models_.insert(it->second);
                            for (const auto& p : c.unresolvedParams)
                                if (!p.name.empty())
                                    generatorParams_[it->second].insert(p.name);
                        }
            // VHDL tops
            std::vector<ir::ModuleId> tops;
            if (!verilogTop) {
                if (!input_.top.empty() || !vhdlEntities.empty())
                    tops = elab.elaborateTops();
            }
            // Verilog tops (explicit, or inferred: not requested from VHDL)
            std::set<std::string> requestedModules;
            for (const auto& r : requests_)
                requestedModules.insert(r.module);
            for (auto t : design->tops) {
                const auto& n = design->instance(t);
                if (n.name == kWrapper)
                    continue;
                if (verilogTop || (input_.top.empty() && !requestedModules.contains(design->body(n.body).name)))
                    tops.insert(tops.begin(), n.body);
            }
            for (const auto& r : requests_)
                if (auto it = primitiveModules_.find(lower(r.module)); it != primitiveModules_.end()) {
                    models_.insert(it->second);
                    for (const auto& p : r.params)
                        if (!p.name.empty())
                            generatorParams_[it->second].insert(p.name);
                }
            // parameters written in another case than the model declares them
            // (Intel megafunctions: Quartus takes .WIDTH for `width`)
            static const std::regex noParamRe("parameter '([^']+)' does not exist in '([^']+)'$");
            for (const auto& d : rs.diags) {
                std::smatch pm;
                if (d.code == "FE-SLANG-ParameterDoesNotExist" && std::regex_search(d.message, pm, noParamRe) &&
                    models_.contains(pm[2].str()))
                    paramAliases_[pm[2].str()].insert(pm[1].str());
            }
            std::string nextSignature;
            for (const auto& m : models_)
                nextSignature += modelText(prims, m);
            lastDiags = std::move(rs.diags);
            // the parameter names of a request take the module's spelling once
            // its parameters are known (Intel megafunctions: .WIDTH for
            // `width`): the wrapper must be the one this round used
            if (requests_.size() == knownRequests && nextSignature == modelSignature && wrapperText() == wrapperUsed) {
                converged = true;
                const bool errors =
                    std::ranges::any_of(lastDiags, [](const Diagnostic& d) { return d.isError(); });
                if (!errors)
                    finalDesign = compact(*design, tops);
                lastDesign = design; // for diagnostics even when the design has errors
            }
        }
        if (!converged && finalDesign == nullptr && !lastDiags.empty() &&
            std::ranges::none_of(lastDiags, [](const Diagnostic& d) { return d.isError(); }))
            lastDiags.push_back({"FE-MIXED", Severity::Error,
                                 "mixed-language elaboration did not converge (Verilog <-> VHDL nesting deeper than " +
                                     std::to_string(kMaxRounds) + " levels?)",
                                 {}, {}});
        // a top that names nothing: suggest the closest VHDL entities / Verilog modules
        for (auto& d : lastDiags) {
            if (d.code != "FE-SLANG-InvalidTopModule" || input_.top.empty())
                continue;
            std::vector<std::string> names;
            for (const auto& v : vhdlDefs)
                names.push_back(v.name);
            for (const auto& v : verilogDefs_)
                names.push_back(v.name);
            const std::string want = lower(input_.top);
            auto distance = [](const std::string& a, const std::string& b) {
                std::vector<std::size_t> row(b.size() + 1);
                for (std::size_t j = 0; j <= b.size(); ++j)
                    row[j] = j;
                for (std::size_t i = 1; i <= a.size(); ++i) {
                    std::size_t diag = row[0];
                    row[0] = i;
                    for (std::size_t j = 1; j <= b.size(); ++j) {
                        const std::size_t up = row[j];
                        row[j] = std::min({row[j] + 1, row[j - 1] + 1, diag + (a[i - 1] == b[j - 1] ? 0U : 1U)});
                        diag = up;
                    }
                }
                return row[b.size()];
            };
            std::vector<std::pair<std::size_t, std::string>> close;
            for (const auto& n : names) {
                const std::size_t dist = distance(want, lower(n));
                if (dist <= std::max<std::size_t>(3, want.size() / 3))
                    close.emplace_back(dist, n);
            }
            std::ranges::sort(close);
            d.message = "top '" + input_.top + "' is neither a VHDL entity nor a Verilog module of the sources";
            if (!close.empty()) {
                std::string list;
                for (std::size_t k = 0; k < close.size() && k < 3; ++k)
                    list += (k ? ", '" : "'") + close[k].second + "'";
                d.notes.push_back("did you mean " + list + "?");
            }
        }
        // names the diagnostics already talk about (unresolved instances below)
        std::set<std::string> mentioned;
        {
            static const std::regex quoted("'([^']+)'");
            for (const auto& d : lastDiags)
                for (std::sregex_iterator it(d.message.begin(), d.message.end(), quoted), end; it != end; ++it)
                    mentioned.insert(lower((*it)[1].str()));
        }
        // Unconnected pins of vendor primitives are normal (an MMCM has 20).
        std::set<std::tuple<std::string, std::uint32_t, std::uint32_t>> primitiveSites;
        if (const ir::Design* sites = finalDesign ? finalDesign.get() : lastDesign.get())
            for (const auto& b : sites->bodies)
                for (const auto& c : b.children)
                    if (models_.contains(c.definition) || primitiveModules_.contains(lower(c.definition)))
                        primitiveSites.emplace(c.loc.file, c.loc.line, c.loc.column);
        // Unmodelled pins: one note per primitive instance (a GT has ~200 pins)
        // instead of one per pin.
        {
            std::vector<Diagnostic> merged;
            std::map<std::string, std::size_t> group; // "module|file|instance line" -> index in merged
            std::map<std::size_t, std::vector<std::string>> pins;
            static const std::regex pinRe("^port '([^']+)' of vendor primitive '([^']+)'");
            for (auto& d : lastDiags) {
                std::smatch m;
                if (d.code != "FE-PRIMITIVE-PORT" || !std::regex_search(d.message, m, pinRe)) {
                    merged.push_back(std::move(d));
                    continue;
                }
                const std::string module = m[2].str();
                const std::string key = module + "|" + d.location.file;
                auto [it, inserted] = group.try_emplace(key, merged.size());
                if (inserted)
                    merged.push_back(d);
                pins[it->second].push_back(m[1].str());
            }
            for (auto& [idx, list] : pins) {
                std::smatch m;
                std::regex_search(merged[idx].message, m, pinRe);
                const std::string module = m[2].str();
                std::string names;
                for (std::size_t k = 0; k < list.size() && k < 8; ++k)
                    names += (k ? ", " : "") + list[k];
                if (list.size() > 8)
                    names += ", ... (" + std::to_string(list.size() - 8) + " more)";
                merged[idx].message = list.size() == 1
                                          ? "port '" + list.front() + "' of vendor primitive '" + module +
                                                "' is not modelled; its connection is ignored"
                                          : std::to_string(list.size()) + " ports of vendor primitive '" + module +
                                                "' are not modelled; their connections are ignored: " + names;
            }
            lastDiags = std::move(merged);
        }
        // pins of primitives are dropped below; the remaining open ports are merged per instance
        std::erase_if(lastDiags, [&](const Diagnostic& d) {
            return (d.code == "FE-SLANG-UnconnectedOutputPort" || d.code == "FE-SLANG-UnconnectedInputPort") &&
                   primitiveSites.contains({d.location.file, d.location.line, d.location.column});
        });
        mergeUnconnectedPorts(lastDiags);
        for (auto& d : lastDiags) {
            if (d.location.file == wrapperUtf8) {
                if (!d.isError())
                    continue;
                // never drop an error: it explains why there is no design
                d.message = "in the generated mixed-language wrapper: " + d.message;
                d.location = {};
            }
            if (d.code == "FE-SLANG-ParameterDoesNotExist") {
                // vendor primitives take many parameters the models do not need
                static const std::regex inRe("in '([^']+)'$");
                std::smatch m;
                if (std::regex_search(d.message, m, inRe) && models_.contains(m[1].str()))
                    continue;
            }
            if ((d.code == "FE-SLANG-UnconnectedOutputPort" || d.code == "FE-SLANG-UnconnectedInputPort") &&
                primitiveSites.contains({d.location.file, d.location.line, d.location.column}))
                continue;
            if (!d.isError() && d.location.file.starts_with(primDirUtf8))
                continue; // notes about the vendor models themselves
            sink_.report(std::move(d));
        }
        if (finalDesign && !models_.empty()) {
            std::string list;
            for (const auto& m : models_)
                list += (list.empty() ? "" : ", ") + m;
            sink_.report({"FE-PRIMITIVES", Severity::Info,
                          "built-in models used for vendor primitives: " + list, {}, {}});
        }
        // Every instance left without a body is reported (never silently a black box)
        if (finalDesign) {
            std::set<std::pair<std::string, std::string>> seen; // definition, location
            for (const auto& b : finalDesign->bodies)
                for (const auto& c : b.children) {
                    if (c.body.valid() || c.definition == "?" || lower(c.definition) == "opencdc_async_source" ||
                        mentioned.contains(lower(c.definition)))
                        continue;
                    if (!seen.emplace(c.definition, toString(c.loc)).second)
                        continue;
                    sink_.report({"FE-UNRESOLVED-INSTANCE", Severity::Warning,
                                  "instance '" + c.name + "' of '" + c.definition +
                                      "' has neither a definition nor a built-in model; it is analyzed as a black box",
                                  c.loc, {}});
                }
        }
        cleanup();

        // summary
        auto& sum = output.summary;
        sum.parsedFiles = parsedVerilog_;
        sum.parsedFiles.insert(sum.parsedFiles.end(), vhdlPaths.begin(), vhdlPaths.end());
        sum.definitions = verilogDefs_;
        sum.definitions.insert(sum.definitions.end(), vhdlDefs.begin(), vhdlDefs.end());
        std::ranges::sort(sum.definitions, {}, &DefinitionInfo::name);
        if (finalDesign) {
            for (auto t : finalDesign->tops)
                sum.topInstances.push_back(makeInfo(*finalDesign, t));
            sum.instanceCount = finalDesign->instances.size();
        }
        output.design = finalDesign;
        return output;
    }

private:
    /// Body of Verilog module `module` with VHDL generic-map values `p`.
    ir::ModuleId resolve(const std::string& module, std::vector<ir::InstanceParam> p) {
        // identity: module and parameter values, names in any case
        std::string key = module;
        for (const auto& x : p)
            key += "|" + lower(x.name) + "=" + verilogValue(x);
        for (std::size_t k = 0; k < requests_.size(); ++k)
            if (requests_[k].text == key)
                return k < resolved_.size() ? resolved_[k] : ir::ModuleId{};
        requests_.push_back({module, std::move(p), key});
        return {};
    }

    /// "#(...)" of a request; VHDL formals take the Verilog spelling (known
    /// once the module has been compiled).
    std::string paramText(const Request& r) const {
        std::string text;
        const bool named = std::ranges::all_of(r.params, [](const auto& x) { return !x.name.empty(); });
        for (const auto& x : r.params) {
            std::string name = x.name;
            if (auto it = verilogParams_.find(r.module); it != verilogParams_.end())
                for (const auto& n : it->second)
                    if (lower(n) == lower(name))
                        name = n;
            if (!text.empty())
                text += ", ";
            text += named ? "." + name + "(" + verilogValue(x) + ")" : verilogValue(x);
        }
        return text.empty() ? std::string() : "#(" + text + ")";
    }

    /// The model of a vendor primitive, with the parameter spellings the
    /// design uses in another case declared as aliases of the model's
    /// parameters: `parameter WIDTH = 1, parameter width = WIDTH`.
    std::string modelText(const PrimitiveLibrary& prims, const std::string& m) {
        std::string text = prims.isClockGenerator(m) ? prims.generatorModel(m, generatorParams_[m]) : prims.verilogModel(m);
        const auto it = paramAliases_.find(m);
        if (it == paramAliases_.end())
            return text;
        for (const auto& alias : it->second) {
            // the declaration of the parameter with the same name in another case
            static const std::regex declRe(R"(parameter\s+((?:integer\s+|real\s+|string\s+)?)([A-Za-z_][A-Za-z0-9_$]*)\s*=)");
            for (auto d = std::sregex_iterator(text.begin(), text.end(), declRe); d != std::sregex_iterator(); ++d) {
                const std::string name = (*d)[2].str();
                if (name == alias || lower(name) != lower(alias))
                    continue;
                const std::string type = (*d)[1].str();
                const auto pos = static_cast<std::size_t>(d->position(0));
                const auto eq = pos + static_cast<std::size_t>(d->length(0));
                // default value: up to the next ',' or ')' at depth 0
                std::size_t e = eq;
                int depth = 0;
                bool str = false;
                for (; e < text.size(); ++e) {
                    const char c = text[e];
                    if (c == '"')
                        str = !str;
                    else if (!str && (c == '(' || c == '{'))
                        ++depth;
                    else if (!str && (c == ')' || c == '}')) {
                        if (depth == 0)
                            break;
                        --depth;
                    }
                    else if (!str && c == ',' && depth == 0)
                        break;
                }
                const std::string def = text.substr(eq, e - eq);
                text.replace(pos, e - pos, "parameter " + type + alias + " =" + def + ", parameter " + type + name +
                                               " = " + alias);
                break;
            }
        }
        return text;
    }

    [[nodiscard]] std::string wrapperText() const {
        std::ostringstream out;
        out << "// generated by OpenCDC: Verilog modules instantiated from VHDL\n";
        out << "module " << kWrapper << ";\n";
        for (std::size_t k = 0; k < requests_.size(); ++k)
            out << "  " << requests_[k].module << " " << paramText(requests_[k]) << " r" << k << " ();\n";
        out << "endmodule\n";
        return out.str();
    }

    void writeWrapper(const std::filesystem::path& path) const {
        std::ofstream out(path, std::ios::binary);
        out << wrapperText();
    }

    /// Binds the unresolved child `ci` of body `b` to VHDL body `id`.
    static void bind(ir::Design& d, std::size_t b, std::size_t ci, ir::ModuleId id,
                     const std::vector<vhdl::Elaborator::ForeignPort>& ports, DiagnosticSink& sink) {
        ir::ChildInstance& c = d.bodies[b].children[ci];
        const auto& childPorts = d.body(id).ports;
        std::vector<ir::SigSpec> actual(ports.size());
        std::vector<bool> given(ports.size(), false);
        for (std::size_t k = 0; k < c.ports.size(); ++k) {
            const auto& pb = c.ports[k];
            std::optional<std::size_t> idx;
            if (pb.port.empty()) {
                if (k < ports.size())
                    idx = k;
            }
            else {
                for (std::size_t i = 0; i < ports.size(); ++i)
                    if (lower(ports[i].name) == lower(pb.port))
                        idx = i;
            }
            if (!idx) {
                sink.report({"FE-MIXED-PORT", Severity::Error,
                             "VHDL entity '" + c.definition + "' has no port '" +
                                 (pb.port.empty() ? std::to_string(k + 1) : pb.port) + "'",
                             c.loc, {}});
                continue;
            }
            actual[*idx] = pb.actual;
            given[*idx] = !pb.actual.empty();
        }
        std::vector<ir::PortBinding> out;
        for (std::size_t i = 0; i < ports.size(); ++i) {
            const auto& fp = ports[i];
            ir::SigSpec a = actual[i];
            if (given[i] && fp.dir == ir::PortDir::In && a.width() != fp.width)
                a = a.resized(fp.width, false); // Verilog port rule: zero extension / truncation
            for (const auto& [pi, off] : fp.pieces) {
                ir::PortBinding pb;
                pb.port = childPorts[pi].name;
                pb.dir = childPorts[pi].dir;
                pb.childNet = childPorts[pi].net;
                const std::uint32_t w = d.body(id).net(childPorts[pi].net).width;
                if (given[i] && off < a.width())
                    pb.actual = a.extract(off, std::min(w, a.width() - off));
                out.push_back(std::move(pb));
            }
        }
        c.ports = std::move(out);
        c.body = id;
        c.unresolvedParams.clear();
    }

    void cleanup() {
        std::error_code ec;
        if (!tmpDir_.empty())
            std::filesystem::remove_all(tmpDir_, ec);
    }

    const FrontendInput& input_;
    DiagnosticSink& sink_;
    std::vector<vhdl::SourceFileEntry> files_;
    std::filesystem::path tmpDir_;
    std::vector<Request> requests_;
    std::vector<ir::ModuleId> resolved_;
    std::map<std::string, std::string> verilogModules_; ///< lower-case -> as written
    std::map<std::string, std::vector<std::string>> verilogParams_;
    std::map<std::string, std::set<std::string>> paramAliases_; ///< model -> parameter spellings of the design
    std::vector<DefinitionInfo> verilogDefs_;
    std::vector<std::filesystem::path> parsedVerilog_;
    std::map<std::string, std::string> primitiveModules_; ///< modelled primitives: lower-case -> name
    std::set<std::string> models_;                        ///< primitive models in the compilation
    std::map<std::string, std::set<std::string>> generatorParams_; ///< clock generator -> parameters in use
    std::vector<std::filesystem::path> vendorVhdl_;       ///< vendor component packages added
};

class AutoFrontend final : public IFrontend {
public:
    Result<FrontendOutput> elaborate(const FrontendInput& original, DiagnosticSink& sink) override {
        // Vivado IP configurations (.xci) are replaced by generated models
        FrontendInput input = original;
        if (std::ranges::any_of(input.sourceFiles,
                                [](const auto& f) { return isXciFile(f) || isBlockDesignFile(f); })) {
            input.sourceFiles.clear();
            input.sourceLibraries.clear();
            XciContext context;
            context.ipRepositories = original.ipRepositories;
            // VHDL component declarations give the ports of an XML .xci without a component file
            for (const auto& f : original.sourceFiles)
                if (isVhdlFile(f))
                    if (const auto t = readSourceText(f))
                        for (auto& [k, c] : vhdlComponentPorts(t->utf8, toUtf8(f)))
                            context.vhdlComponents.emplace(k, std::move(c));
            // sources in order; an IP from a repository adds its own files
            // (which may contain further .xci) right after its configuration
            std::vector<std::pair<std::filesystem::path, std::string>> queue;
            for (std::size_t i = 0; i < original.sourceFiles.size(); ++i)
                queue.emplace_back(original.sourceFiles[i],
                                   i < original.sourceLibraries.size() ? original.sourceLibraries[i] : std::string());
            std::set<std::string> seen;
            for (std::size_t q = 0; q < queue.size(); ++q) {
                const auto [f, lib] = queue[q];
                std::error_code ec;
                if (!seen.insert(std::filesystem::weakly_canonical(f, ec).string()).second)
                    continue; // the same file listed twice (an IP used by several others)
                if (isBlockDesignFile(f)) {
                    // a block design: its module, then the configurations of its IP cells
                    auto bd = blockDesignFromFile(f);
                    if (!bd) {
                        sink.report(std::move(bd).error());
                        continue;
                    }
                    const auto& b = bd.value();
                    const auto path = primitiveDir() / (b.name + "_bd_" +
                                                        std::to_string(std::hash<std::string>{}(b.verilog) % 1000000) + ".v");
                    writeIfChanged(path, b.verilog);
                    input.sourceFiles.push_back(path);
                    input.sourceLibraries.push_back(std::string());
                    for (std::size_t k = 0; k < b.ipFiles.size(); ++k)
                        queue.insert(queue.begin() + static_cast<std::ptrdiff_t>(q + 1 + k), {b.ipFiles[k], std::string()});
                    sink.report({"FE-BD-MODULE", Severity::Info,
                                 "block design '" + b.name + "': module '" + b.name + "' with " +
                                     std::to_string(b.cells) + " cell(s)" +
                                     (b.hierarchies ? ", " + std::to_string(b.hierarchies) + " hierarchical cell(s) flattened"
                                                    : std::string()) +
                                     ", " + std::to_string(b.ipFiles.size()) + " IP configuration(s)",
                                 SourceLocation{toUtf8(f), 0, 0}, b.notes});
                    continue;
                }
                if (!isXciFile(f)) {
                    input.sourceFiles.push_back(f);
                    input.sourceLibraries.push_back(lib);
                    if (const auto ext = f.extension().string(); (ext == ".sv" || ext == ".svh") &&
                                                                 input.language != LanguageStd::SystemVerilog2017)
                        input.language = LanguageStd::SystemVerilog2017; // an IP's SystemVerilog sources
                    continue;
                }
                auto model = modelFromXci(f, context);
                if (!model) {
                    sink.report(std::move(model).error()); // the IP stays an unknown module
                    continue;
                }
                const auto& mv = model.value();
                const auto path = primitiveDir() / (mv.module + "_xci_" +
                                                    std::to_string(std::hash<std::string>{}(mv.verilog) % 1000000) + ".v");
                writeIfChanged(path, mv.verilog);
                input.sourceFiles.push_back(path);
                input.sourceLibraries.push_back(std::string());
                for (std::size_t k = 0; k < mv.extraSources.size(); ++k)
                    queue.insert(queue.begin() + static_cast<std::ptrdiff_t>(q + 1 + k),
                                 {mv.extraSources[k], k < mv.extraLibraries.size() ? mv.extraLibraries[k] : std::string()});
                Diagnostic d{"FE-XCI-MODEL", Severity::Info,
                             "IP '" + mv.module + "' (" + mv.ip + ") modelled from its configuration: " + mv.summary,
                             SourceLocation{toUtf8(f), 0, 0}, mv.notes};
                sink.report(std::move(d));
            }
        }
        const auto vhdl = std::ranges::count_if(input.sourceFiles, isVhdlFile);
        if (vhdl == 0) {
            // Verilog only: instances of modelled vendor primitives are
            // elaborated again with the models.
            RoundSink first;
            auto r = makeSlangFrontend()->elaborate(input, first);
            if (r.ok() && !r.value().design &&
                (primitivesMissing(first.diags) || (input.blackBoxUnknownModules && unknownModules(first.diags)))) {
                MixedLinker linker(input, sink);
                return linker.run();
            }
            mergeUnconnectedPorts(first.diags);
            for (auto& d : first.diags)
                sink.report(std::move(d));
            return r;
        }
        if (static_cast<std::size_t>(vhdl) == input.sourceFiles.size() && !usesVendorLibrary(input)) {
            // VHDL only: a component declared locally for a modelled vendor
            // primitive (no vendor library clause) is elaborated again with the models
            RoundSink first;
            auto r = makeVhdlFrontend()->elaborate(input, first);
            if (r.ok() && unresolvedPrimitive(first.diags)) {
                MixedLinker linker(input, sink);
                return linker.run();
            }
            for (auto& d : first.diags)
                sink.report(std::move(d));
            return r;
        }
        MixedLinker linker(input, sink);
        return linker.run();
    }

private:
    /// Some unknown module is a modelled vendor primitive (the linker then
    /// reports the remaining unknown modules itself).
    static bool primitivesMissing(const std::vector<Diagnostic>& diags) {
        static const std::regex nameRe("'([^']+)'");
        for (const auto& d : diags) {
            std::smatch m;
            // the built-in black box of the models (IP models from .xci use it too)
            if (d.code == "FE-SLANG-UnknownModule" && std::regex_search(d.message, m, nameRe) &&
                (PrimitiveLibrary::instance().find(m[1].str()) || lower(m[1].str()) == "opencdc_async_source"))
                return true;
        }
        return false;
    }

    static bool unknownModules(const std::vector<Diagnostic>& diags) {
        return std::ranges::any_of(diags, [](const Diagnostic& d) { return d.code == "FE-SLANG-UnknownModule"; });
    }

    /// A VHDL instance left as a black box names a modelled vendor primitive.
    static bool unresolvedPrimitive(const std::vector<Diagnostic>& diags) {
        static const std::regex nameRe("entity/component '([^']+)'");
        for (const auto& d : diags) {
            std::smatch m;
            if (d.code == "IR-UNRESOLVED-INSTANCE" && std::regex_search(d.message, m, nameRe) &&
                PrimitiveLibrary::instance().find(m[1].str()))
                return true;
        }
        return false;
    }

    static bool usesVendorLibrary(const FrontendInput& input) {
        static const std::regex libRe = [] {
            std::set<std::string> libs;
            for (const auto& pkg : PrimitiveLibrary::instance().vhdlPackages())
                libs.insert(pkg.library);
            std::string alt;
            for (const auto& l : libs)
                alt += (alt.empty() ? "" : "|") + l;
            return std::regex(R"((^|[^A-Za-z0-9_])library\s+[^;]*\b()" + alt + R"()\b)", std::regex::icase);
        }();
        for (const auto& f : input.sourceFiles) {
            std::ifstream in(f, std::ios::binary);
            std::ostringstream ss;
            ss << in.rdbuf();
            const std::string text = withoutVhdlComments(ss.str());
            if (std::regex_search(text, libRe))
                return true;
        }
        return false;
    }
};

} // namespace

std::unique_ptr<IFrontend> makeDefaultFrontend() {
    return std::make_unique<AutoFrontend>();
}

} // namespace opencdc::frontend
