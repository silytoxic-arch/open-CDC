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
#include "frontend/Frontend.h"
#include "frontend/MixedSupport.h"
#include "frontend/vhdl/VhdlElab.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>

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

struct Request {
    std::string module;
    std::vector<ir::InstanceParam> params;
    std::string text; ///< "#(...)" as written into the wrapper
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
        static std::atomic<unsigned> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        tmpDir_ = std::filesystem::temp_directory_path() /
                  ("opencdc_mixed_" + std::to_string(stamp) + "_" + std::to_string(counter++));
        std::error_code ec;
        std::filesystem::create_directories(tmpDir_, ec);
        const auto wrapperPath = tmpDir_ / "opencdc_mixed_wrapper.v";
        vin.sourceFiles.push_back(wrapperPath);
        const std::string wrapperUtf8 = toUtf8(std::filesystem::weakly_canonical(wrapperPath, ec));

        std::vector<Diagnostic> lastDiags;
        std::shared_ptr<ir::Design> finalDesign;
        bool converged = false;
        for (int round = 0; round < kMaxRounds && !converged; ++round) {
            writeWrapper(wrapperPath);
            RoundSink rs;
            detail::VerilogMixedOptions mo;
            mo.foreignModules = vhdlEntities;
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
            if (round == 0) {
                verilogParams_ = params;
                for (const auto& d : vout.value().summary.definitions)
                    if (d.name != kWrapper) {
                        verilogDefs_.push_back(d);
                        verilogModules_[lower(d.name)] = d.name;
                    }
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
            lastDiags = std::move(rs.diags);
            if (requests_.size() == knownRequests) {
                converged = true;
                const bool errors =
                    std::ranges::any_of(lastDiags, [](const Diagnostic& d) { return d.isError(); });
                if (!errors)
                    finalDesign = compact(*design, tops);
            }
        }
        if (!converged && finalDesign == nullptr && !lastDiags.empty() &&
            std::ranges::none_of(lastDiags, [](const Diagnostic& d) { return d.isError(); }))
            lastDiags.push_back({"FE-MIXED", Severity::Error,
                                 "mixed-language elaboration did not converge (Verilog <-> VHDL nesting deeper than " +
                                     std::to_string(kMaxRounds) + " levels?)",
                                 {}, {}});
        for (auto& d : lastDiags) {
            if (d.location.file == wrapperUtf8)
                continue;
            sink_.report(std::move(d));
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
        // VHDL formals are case-insensitive: use the Verilog spelling
        if (auto it = verilogParams_.find(module); it != verilogParams_.end())
            for (auto& x : p)
                for (const auto& name : it->second)
                    if (!x.name.empty() && lower(name) == lower(x.name))
                        x.name = name;
        std::string text;
        const bool named = std::ranges::all_of(p, [](const auto& x) { return !x.name.empty(); });
        for (const auto& x : p) {
            if (!text.empty())
                text += ", ";
            text += named ? "." + x.name + "(" + verilogValue(x) + ")" : verilogValue(x);
        }
        text = text.empty() ? std::string() : "#(" + text + ")";
        for (std::size_t k = 0; k < requests_.size(); ++k)
            if (requests_[k].module == module && requests_[k].text == text)
                return k < resolved_.size() ? resolved_[k] : ir::ModuleId{};
        requests_.push_back({module, std::move(p), text});
        return {};
    }

    void writeWrapper(const std::filesystem::path& path) const {
        std::ofstream out(path, std::ios::binary);
        out << "// generated by OpenCDC: Verilog modules instantiated from VHDL\n";
        out << "module " << kWrapper << ";\n";
        for (std::size_t k = 0; k < requests_.size(); ++k)
            out << "  " << requests_[k].module << " " << requests_[k].text << " r" << k << " ();\n";
        out << "endmodule\n";
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
    std::vector<DefinitionInfo> verilogDefs_;
    std::vector<std::filesystem::path> parsedVerilog_;
};

class AutoFrontend final : public IFrontend {
public:
    Result<FrontendOutput> elaborate(const FrontendInput& input, DiagnosticSink& sink) override {
        const auto vhdl = std::ranges::count_if(input.sourceFiles, isVhdlFile);
        if (vhdl == 0)
            return makeSlangFrontend()->elaborate(input, sink);
        if (static_cast<std::size_t>(vhdl) == input.sourceFiles.size())
            return makeVhdlFrontend()->elaborate(input, sink);
        MixedLinker linker(input, sink);
        return linker.run();
    }
};

} // namespace

std::unique_ptr<IFrontend> makeDefaultFrontend() {
    return std::make_unique<AutoFrontend>();
}

} // namespace opencdc::frontend
