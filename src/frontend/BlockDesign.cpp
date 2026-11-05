#include "frontend/BlockDesign.h"

#include "core/PathUtil.h"
#include "core/TextEncoding.h"
#include "frontend/XciModels.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>

namespace opencdc::frontend {

namespace {

using json = nlohmann::json;

std::string lowerStr(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string join(const std::string& path, const std::string& x) {
    return path.empty() ? x : path + "/" + x;
}

/// A Verilog identifier from a cell path or net name ("sys_bus/xbar" -> "sys_bus_xbar").
std::string ident(const std::string& s) {
    std::string out;
    for (const char c : s)
        out += std::isalnum(static_cast<unsigned char>(c)) || c == '_' ? c : '_';
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0])))
        out = "n_" + out;
    return out;
}

long long jsonNum(const json& n) {
    return n.is_string() ? std::stoll(n.get<std::string>()) : n.get<long long>();
}

struct Port {
    std::string name;
    char dir = 'i'; ///< 'i', 'o', 'b' (inout)
    std::optional<std::pair<long long, long long>> range;
    unsigned width() const { return range ? static_cast<unsigned>(std::llabs(range->first - range->second) + 1) : 1U; }
};

struct Leaf {
    std::string path;     ///< "sys_bus/s00_couplers/auto_cc"
    std::string instance; ///< "sys_bus_s00_couplers_auto_cc"
    std::string module;
    bool moduleRef = false;
    std::vector<std::pair<std::string, std::string>> parameters;
    std::vector<Port> ports;
    std::map<std::string, std::size_t> portIndex;
    std::map<std::string, std::vector<XciPortMap>> interfaces; ///< upper-case interface name -> port map
    std::map<std::string, long long> defaults;
};

/// A connection point: a whole port, or bits [lo, hi] (from its least significant bit) of a leaf port.
struct Term {
    bool top = false;
    std::size_t leaf = 0; ///< leaf index, or top port index
    std::size_t port = 0;
    unsigned lo = 0, hi = 0;
    bool whole = true;
};

class UnionFind {
public:
    std::size_t id(const std::string& key) {
        auto [it, added] = ids_.try_emplace(key, parent_.size());
        if (added)
            parent_.push_back(parent_.size());
        return it->second;
    }
    std::size_t find(std::size_t x) {
        while (parent_[x] != x)
            x = parent_[x] = parent_[parent_[x]];
        return x;
    }
    void unite(const std::string& a, const std::string& b) {
        const auto ra = find(id(a)), rb = find(id(b));
        if (ra != rb)
            parent_[std::max(ra, rb)] = std::min(ra, rb);
    }
    std::optional<std::size_t> lookup(const std::string& key) {
        const auto it = ids_.find(key);
        if (it == ids_.end())
            return std::nullopt;
        return find(it->second);
    }

private:
    std::map<std::string, std::size_t> ids_;
    std::vector<std::size_t> parent_;
};

class Builder {
public:
    Builder(const std::filesystem::path& dir) : dir_(dir) {}

    Result<BlockDesign> run(const json& design) {
        if (design.contains("design_info") && design["design_info"].is_object())
            out_.name = design["design_info"].value("name", std::string());
        if (out_.name.empty())
            return Diagnostic{"FE-BD-PARSE", Severity::Error, "not a Vivado block design (no design name)", {}, {}};
        // top ports
        if (design.contains("ports"))
            for (const auto& [name, p] : design["ports"].items()) {
                Port tp;
                tp.name = name;
                tp.dir = dirOf(p.value("direction", std::string("I")));
                if (p.contains("left") && p.contains("right"))
                    tp.range = std::pair{jsonNum(p["left"]), jsonNum(p["right"])};
                addTop(std::move(tp));
            }
        if (design.contains("interface_ports"))
            for (const auto& [name, p] : design["interface_ports"].items()) {
                (void)p;
                topInterfaces_.push_back(name);
                ifs_.id(name);
                ifKeys_.push_back(name);
            }
        walk(design, "", 0);
        if (!missing_.empty())
            return Diagnostic{"FE-BD-CELL", Severity::Error,
                              "block design '" + out_.name + "': the ports of cell(s) " + missing_ + " are not known",
                              {},
                              {"the component file ip/<cell>/<cell>.xml (or a JSON .xci with its boundary) is "
                               "written by Vivado with the IP's output products: generate them (Generate Output "
                               "Products / generate_target) and keep the ip folder next to the .bd"}};
        expandInterfaces();
        emit();
        return std::move(out_);
    }

private:
    static char dirOf(const std::string& d) {
        const std::string l = lowerStr(d);
        return l == "o" || l == "out" ? 'o' : l == "io" || l == "inout" ? 'b' : 'i';
    }

    void addTop(Port p) {
        const std::size_t idx = topPorts_.size();
        topIndex_[p.name] = idx;
        addTerm({true, idx, 0, 0, p.width() - 1, true}, p.name);
        topPorts_.push_back(std::move(p));
    }

    std::optional<XciCellInterface> cellInterface(const json& c, const std::string& xci) {
        if (!xci.empty()) {
            const auto base = dir_ / "ip" / xci;
            for (const char* ext : {".xml", ".xci"})
                if (const auto t = readSourceText(base / (xci + ext)))
                    if (auto ci = cellInterfaceFromText(t->utf8))
                        return ci;
        }
        // a module reference: the ports and port maps in the block design itself
        if (!c.contains("ports") && !c.contains("interface_ports"))
            return std::nullopt;
        XciCellInterface ci;
        std::set<std::string> seen;
        auto addPort = [&](const std::string& name, const json& p) {
            if (!seen.insert(name).second)
                return;
            XciPort x;
            x.name = name;
            const char d = dirOf(p.value("direction", std::string("I")));
            x.output = d == 'o';
            x.inout = d == 'b';
            if (p.contains("left") && p.contains("right"))
                x.range = std::pair{jsonNum(p["left"]), jsonNum(p["right"])};
            ci.ports.push_back(std::move(x));
        };
        if (c.contains("ports"))
            for (const auto& [name, p] : c["ports"].items())
                addPort(name, p);
        if (c.contains("interface_ports"))
            for (const auto& [ifName, ip] : c["interface_ports"].items()) {
                if (!ip.contains("port_maps"))
                    continue;
                XciBusInterface bi;
                bi.name = ifName;
                for (const auto& [logical, m] : ip["port_maps"].items()) {
                    if (!m.contains("physical_name"))
                        continue;
                    const std::string phys = m["physical_name"].get<std::string>();
                    addPort(phys, m);
                    bi.ports.push_back({logical, phys, std::nullopt});
                }
                ci.interfaces.push_back(std::move(bi));
            }
        if (ci.ports.empty())
            return std::nullopt;
        return ci;
    }

    void walk(const json& level, const std::string& path, int depth) {
        if (level.contains("components"))
            for (const auto& [cell, c] : level["components"].items()) {
                const std::string full = join(path, cell);
                // a hierarchy (also an AXI Interconnect, and the couplers that only pass the interface on)
                if (c.contains("components") || (!c.contains("vlnv") && !c.contains("xci_name"))) {
                    ++out_.hierarchies;
                    hierarchies_.push_back(full);
                    walk(c, full, depth + 1);
                    continue;
                }
                const std::string vlnv = c.value("vlnv", std::string());
                const std::string xci = c.value("xci_name", std::string());
                Leaf leaf;
                leaf.path = full;
                leaf.instance = ident(cell);
                leaf.moduleRef = vlnv.find(":module_ref:") != std::string::npos;
                if (leaf.moduleRef && c.contains("reference_info"))
                    leaf.module = c["reference_info"].value("ref_name", std::string());
                else
                    leaf.module = xci;
                if (leaf.module.empty()) {
                    out_.notes.push_back("cell '" + full + "' (" + vlnv + ") has no IP configuration: left out");
                    continue;
                }
                auto ci = cellInterface(c, xci);
                if (!ci) {
                    missing_ += (missing_.empty() ? "'" : ", '") + full + "'";
                    continue;
                }
                if (!leaf.moduleRef) {
                    const auto base = dir_ / "ip" / xci;
                    std::filesystem::path cfg;
                    for (const char* ext : {".xci", ".xcix"})
                        if (std::error_code ec; std::filesystem::exists(base / (xci + ext), ec))
                            cfg = base / (xci + ext);
                    if (cfg.empty())
                        out_.notes.push_back("cell '" + full + "': no configuration ip/" + xci + "/" + xci +
                                             ".xci, the IP is not modelled");
                    else
                        out_.ipFiles.push_back(cfg);
                }
                else {
                    leaf.parameters = ci->parameters;
                }
                for (auto& p : ci->ports) {
                    Port lp;
                    lp.name = p.name;
                    lp.dir = p.output ? 'o' : p.inout ? 'b' : 'i';
                    lp.range = p.range;
                    leaf.portIndex[p.name] = leaf.ports.size();
                    leaf.ports.push_back(std::move(lp));
                }
                for (auto& bi : ci->interfaces)
                    leaf.interfaces[upper(bi.name)] = std::move(bi.ports);
                leaf.defaults = std::move(ci->defaults);
                const std::size_t li = leaves_.size();
                for (std::size_t k = 0; k < leaf.ports.size(); ++k) {
                    addTerm({false, li, k, 0, leaf.ports[k].width() - 1, true}, join(full, leaf.ports[k].name));
                }
                ++out_.cells;
                leaves_.push_back(std::move(leaf));
            }
        if (level.contains("nets"))
            for (const auto& [name, n] : level["nets"].items()) {
                if (!n.contains("ports"))
                    continue;
                std::string firstKey;
                for (const auto& r : n["ports"]) {
                    const std::string key = join(path, r.get<std::string>());
                    if (firstKey.empty())
                        firstKey = key;
                    else
                        phys_.unite(firstKey, key);
                }
                if (!firstKey.empty())
                    netNames_.push_back({depth, ident(join(path, name)), firstKey});
            }
        if (level.contains("interface_nets"))
            for (const auto& [name, n] : level["interface_nets"].items()) {
                if (!n.contains("interface_ports"))
                    continue;
                std::string firstKey;
                for (const auto& r : n["interface_ports"]) {
                    const std::string key = join(path, r.get<std::string>());
                    ifKeys_.push_back(key);
                    if (firstKey.empty())
                        firstKey = key;
                    else
                        ifs_.unite(firstKey, key);
                }
                if (!firstKey.empty())
                    ifNames_.push_back({depth, ident(join(path, name)), firstKey});
            }
    }

    static std::string upper(std::string s) {
        std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return s;
    }

    /// The term of a port map entry (a new term for a part of a port).
    std::optional<std::string> mapTerm(std::size_t li, const XciPortMap& pm) {
        const Leaf& leaf = leaves_[li];
        const auto it = leaf.portIndex.find(pm.physical);
        if (it == leaf.portIndex.end())
            return std::nullopt; // a port the configuration disables
        const Port& p = leaf.ports[it->second];
        const std::string whole = join(leaf.path, p.name);
        if (!pm.slice || !p.range)
            return whole;
        const long long base = std::min(p.range->first, p.range->second);
        const long long a = pm.slice->first - base, b = pm.slice->second - base;
        const unsigned lo = static_cast<unsigned>(std::min(a, b)), hi = static_cast<unsigned>(std::max(a, b));
        if (lo == 0 && hi + 1 == p.width())
            return whole;
        if (hi + 1 > p.width())
            return std::nullopt;
        const std::string key = whole + "[" + std::to_string(hi) + ":" + std::to_string(lo) + "]";
        if (!phys_.lookup(key)) {
            addTerm({false, li, it->second, lo, hi, false}, key);
        }
        return key;
    }

    void expandInterfaces() {
        // interface endpoints of the leaves and of the design
        struct Endpoint {
            bool top = false;
            std::size_t leaf = 0;
            std::string name; ///< interface name
        };
        std::map<std::size_t, std::vector<Endpoint>> groups;
        for (std::size_t li = 0; li < leaves_.size(); ++li)
            for (const auto& [ifName, map] : leaves_[li].interfaces) {
                (void)map;
                // the block design spells the interface as the component does, maybe in another case
                if (const auto g = lookupInterface(leaves_[li].path, ifName))
                    groups[*g].push_back({false, li, ifName});
            }
        for (const auto& t : topInterfaces_)
            if (const auto g = ifs_.lookup(t))
                groups[*g].push_back({true, 0, t});
        std::map<std::size_t, std::string> groupName;
        for (const auto& n : ifNames_)
            if (const auto g = ifs_.lookup(n.key))
                if (!groupName.contains(*g) || n.depth < bestIfDepth_[*g]) {
                    groupName[*g] = n.name;
                    bestIfDepth_[*g] = n.depth;
                }
        for (const auto& [g, eps] : groups) {
            std::map<std::string, std::vector<std::string>> byLogical; // upper-case logical -> terms
            std::vector<std::string> order;
            for (const auto& ep : eps) {
                if (ep.top)
                    continue;
                for (const auto& pm : leaves_[ep.leaf].interfaces[ep.name]) {
                    const auto key = mapTerm(ep.leaf, pm);
                    if (!key)
                        continue;
                    const std::string l = upper(pm.logical);
                    if (!byLogical.contains(l))
                        order.push_back(l);
                    byLogical[l].push_back(*key);
                }
            }
            for (const auto& ep : eps) {
                if (!ep.top)
                    continue;
                // the design's interface port: one port per signal, <interface>_<signal>
                for (const auto& l : order) {
                    bool driven = false, inout = false;
                    unsigned width = 1;
                    for (const auto& k : byLogical[l]) {
                        const Term& t = terms_[*phys_index(k)];
                        const Port& p = leaves_[t.leaf].ports[t.port];
                        driven = driven || p.dir == 'o';
                        inout = inout || p.dir == 'b';
                        width = std::max(width, t.hi - t.lo + 1);
                    }
                    Port tp;
                    tp.name = ep.name + "_" + lowerStr(l);
                    if (topIndex_.contains(tp.name))
                        continue;
                    tp.dir = inout ? 'b' : driven ? 'o' : 'i';
                    if (width > 1)
                        tp.range = std::pair{static_cast<long long>(width) - 1, 0LL};
                    addTop(tp);
                    byLogical[l].push_back(tp.name);
                }
            }
            const std::string base = groupName.contains(g) ? groupName[g] : std::string("intf");
            for (const auto& l : order) {
                const auto& keys = byLogical[l];
                for (std::size_t k = 1; k < keys.size(); ++k)
                    phys_.unite(keys[0], keys[k]);
                netNames_.push_back({bestIfDepth_.contains(g) ? bestIfDepth_[g] : 0, ident(base + "_" + l), keys[0]});
            }
        }
    }

    std::optional<std::size_t> lookupInterface(const std::string& path, const std::string& ifName) {
        if (auto g = ifs_.lookup(join(path, ifName)))
            return g;
        // the case the block design uses may differ from the component file's
        const auto it = ifCase_.find(lowerStr(join(path, ifName)));
        if (it == ifCase_.end()) {
            if (!ifCaseBuilt_) {
                ifCaseBuilt_ = true;
                for (const auto& n : allIfKeys())
                    ifCase_[lowerStr(n)] = n;
                return lookupInterface(path, ifName);
            }
            return std::nullopt;
        }
        return ifs_.lookup(it->second);
    }

    std::vector<std::string> allIfKeys() const { return ifKeys_; }

    std::optional<std::size_t> phys_index(const std::string& key) {
        const auto it = termIndex_.find(key);
        if (it == termIndex_.end())
            return std::nullopt;
        return it->second;
    }

    void addTerm(Term t, std::string key) {
        termIndex_[key] = terms_.size();
        terms_.push_back(t);
        phys_.id(key);
        termKey_.push_back(std::move(key));
    }

    unsigned termWidth(const Term& t) const { return t.hi - t.lo + 1; }

    char termDir(const Term& t) const { return t.top ? topPorts_[t.leaf].dir : leaves_[t.leaf].ports[t.port].dir; }

    /// The low `bits` bits of a net.
    static std::string lowBits(const std::string& net, const std::optional<std::pair<long long, long long>>& range,
                               unsigned bits) {
        const long long l = range ? range->first : 0, r = range ? range->second : 0;
        if (l >= r)
            return net + "[" + std::to_string(r + bits - 1) + ":" + std::to_string(r) + "]";
        return net + "[" + std::to_string(r - bits + 1) + ":" + std::to_string(r) + "]";
    }

    static std::string parentOf(const std::string& path) {
        const auto slash = path.rfind('/');
        return slash == std::string::npos ? std::string() : path.substr(0, slash);
    }

    /// `path` is `module` or inside it ("" is the design).
    static bool within(const std::string& path, const std::string& module) {
        return module.empty() || path == module || path.starts_with(module + "/");
    }

    std::string moduleName(const std::string& hier) const {
        return hier.empty() ? out_.name : ident(out_.name + "_" + hier) + "_imp";
    }

    void emit() {
        // the nets: classes of terms
        std::map<std::size_t, std::vector<std::size_t>> classes;
        for (std::size_t i = 0; i < terms_.size(); ++i)
            classes[*phys_.lookup(termKey_[i])].push_back(i);
        std::map<std::size_t, std::pair<int, std::string>> names;
        for (const auto& n : netNames_)
            if (const auto c = phys_.lookup(n.key)) {
                auto it = names.find(*c);
                if (it == names.end() || n.depth < it->second.first)
                    names[*c] = {n.depth, n.name};
            }
        struct Net {
            std::string name;
            unsigned width = 1;
            std::optional<std::pair<long long, long long>> range; ///< of a design port
            bool connected = false;
            bool topPort = false;
            std::vector<std::string> modules; ///< where its terms are (the leaf's hierarchy, "" for design ports)
            std::vector<std::string> driverModules;
            bool inout = false;
            bool undriven = false;
        };
        std::map<std::size_t, Net> nets;
        std::set<std::string> used;
        for (const auto& p : topPorts_)
            used.insert(p.name);
        for (const auto& l : leaves_)
            used.insert(l.instance);
        for (const auto& h : hierarchies_)
            used.insert(ident(h.substr(h.rfind('/') + 1)));
        std::ostringstream topAssigns;
        std::vector<std::string> multi, undriven;
        for (const auto& [root, members] : classes) {
            Net n;
            n.connected = members.size() > 1;
            std::vector<std::size_t> drivers, tops;
            for (const auto i : members) {
                const Term& t = terms_[i];
                const std::string m = t.top ? std::string() : parentOf(leaves_[t.leaf].path);
                n.modules.push_back(m);
                if (t.top)
                    tops.push_back(i);
                const char d = termDir(t);
                if ((t.top && d == 'i') || (!t.top && d == 'o')) {
                    drivers.push_back(i);
                    n.driverModules.push_back(m);
                }
                n.inout = n.inout || d == 'b';
            }
            if (!tops.empty()) {
                const Port& p = topPorts_[terms_[tops[0]].leaf];
                n.name = p.name;
                n.width = p.width();
                n.range = p.range;
                n.topPort = true;
                for (std::size_t k = 1; k < tops.size(); ++k) {
                    const Port& q = topPorts_[terms_[tops[k]].leaf];
                    if (q.dir == 'o')
                        topAssigns << "  assign " << q.name << " = " << n.name << ";\n";
                }
            }
            else {
                std::string base = names.contains(root) ? names[root].second : std::string("net");
                std::string name = base;
                for (int k = 1; used.contains(name); ++k)
                    name = base + "_" + std::to_string(k);
                used.insert(name);
                n.name = name;
                n.width = drivers.empty() ? 1U : termWidth(terms_[drivers[0]]);
                if (drivers.empty())
                    for (const auto i : members)
                        n.width = std::max(n.width, termWidth(terms_[i]));
            }
            if (n.connected && drivers.size() > 1) {
                std::string who;
                for (const auto i : drivers)
                    who += (who.empty() ? "" : ", ") + termKey_[i];
                multi.push_back(n.name + " (" + who + ")");
            }
            if (n.connected && drivers.empty() && !n.inout) {
                n.undriven = !n.topPort;
                undriven.push_back(n.name);
            }
            nets[root] = n;
        }
        auto usedIn = [](const Net& n, const std::string& mod) {
            return std::ranges::any_of(n.modules, [&](const std::string& m) { return within(m, mod); });
        };
        auto crosses = [&](const Net& n, const std::string& mod) {
            return !mod.empty() && usedIn(n, mod) &&
                   std::ranges::any_of(n.modules, [&](const std::string& m) { return !within(m, mod); });
        };
        // the hierarchies directly in a module
        auto children = [&](const std::string& mod) {
            std::vector<std::string> out;
            for (const auto& h : hierarchies_)
                if (parentOf(h) == mod)
                    out.push_back(h);
            return out;
        };
        int dummy = 0;
        std::string text;
        std::vector<std::string> modules = {""};
        modules.insert(modules.end(), hierarchies_.begin(), hierarchies_.end());
        for (const auto& mod : modules) {
            const auto kids = children(mod);
            std::ostringstream wires, assigns, inst;
            std::vector<std::string> ports; // of a hierarchy
            // the nets this module needs: its ports, and the nets its own cells or several children share
            std::map<std::size_t, std::optional<std::pair<long long, long long>>> range; // as declared here
            for (const auto& [root, n] : nets) {
                if (!n.connected || !usedIn(n, mod))
                    continue;
                const bool direct = std::ranges::find(n.modules, mod) != n.modules.end();
                std::size_t inKids = 0;
                for (const auto& k : kids)
                    inKids += usedIn(n, k) ? 1U : 0U;
                const bool port = crosses(n, mod);
                if (!port && !direct && inKids < 2)
                    continue; // inside one child only
                const std::string decl = "[" + std::to_string(n.width - 1) + ":0] " + n.name;
                if (mod.empty() && n.topPort) {
                    range[root] = n.range;
                }
                else if (port) {
                    const bool drivenInside = std::ranges::any_of(n.driverModules, [&](const std::string& m) {
                        return within(m, mod);
                    });
                    ports.push_back(std::string(n.inout ? "inout  wire " : drivenInside ? "output wire " : "input  wire ") +
                                    decl);
                    range[root] = std::pair{static_cast<long long>(n.width) - 1, 0LL};
                }
                else {
                    wires << "  wire " << decl << ";\n";
                    range[root] = std::pair{static_cast<long long>(n.width) - 1, 0LL};
                    if (n.undriven)
                        assigns << "  assign " << n.name << " = " << n.width << "'d0;\n";
                }
            }
            auto netExpr = [&](std::size_t root) { return nets[root].name; };
            // leaf cells
            for (std::size_t li = 0; li < leaves_.size(); ++li) {
                const Leaf& leaf = leaves_[li];
                if (parentOf(leaf.path) != mod)
                    continue;
                std::vector<std::vector<std::size_t>> parts(leaf.ports.size());
                for (std::size_t i = 0; i < terms_.size(); ++i)
                    if (!terms_[i].top && terms_[i].leaf == li)
                        parts[terms_[i].port].push_back(i);
                inst << "  " << leaf.module;
                if (!leaf.parameters.empty()) {
                    inst << " #(\n";
                    for (std::size_t k = 0; k < leaf.parameters.size(); ++k)
                        inst << "    ." << leaf.parameters[k].first << "(" << leaf.parameters[k].second << ")"
                             << (k + 1 < leaf.parameters.size() ? ",\n" : "\n");
                    inst << "  )";
                }
                inst << " " << leaf.instance << " (\n";
                for (std::size_t k = 0; k < leaf.ports.size(); ++k) {
                    const Port& p = leaf.ports[k];
                    const unsigned L = p.width();
                    auto fit = [&](std::size_t root, unsigned bits) -> std::string {
                        const Net& n = nets[root];
                        const std::string name = netExpr(root);
                        if (bits == n.width)
                            return name;
                        if (bits < n.width)
                            return lowBits(name, range[root], bits);
                        if (p.dir == 'i')
                            return "{" + std::to_string(bits - n.width) + "'d0, " + name + "}";
                        const std::string d = "oc_nc_" + std::to_string(dummy++);
                        wires << "  wire [" << bits - n.width - 1 << ":0] " << d << ";\n";
                        return "{" + d + ", " + name + "}";
                    };
                    std::string expr;
                    std::optional<std::size_t> whole;
                    std::vector<std::size_t> pieces;
                    for (const auto i : parts[k]) {
                        if (!nets[*phys_.lookup(termKey_[i])].connected)
                            continue;
                        if (terms_[i].whole)
                            whole = i;
                        else
                            pieces.push_back(i);
                    }
                    if (!pieces.empty()) {
                        if (whole)
                            out_.notes.push_back("port '" + leaf.path + "/" + p.name +
                                                 "' is connected both as a whole and in parts: the parts are used");
                        std::ranges::sort(pieces,
                                          [&](std::size_t a, std::size_t b) { return terms_[a].lo > terms_[b].lo; });
                        std::vector<std::string> items;
                        unsigned next = L; // bits above `next` are done
                        auto gap = [&](unsigned from, unsigned to) { // bits [from, to)
                            if (to <= from)
                                return;
                            if (p.dir == 'i') {
                                items.push_back(std::to_string(to - from) + "'d0");
                            }
                            else {
                                const std::string d = "oc_nc_" + std::to_string(dummy++);
                                wires << "  wire [" << to - from - 1 << ":0] " << d << ";\n";
                                items.push_back(d);
                            }
                        };
                        for (const auto i : pieces) {
                            const Term& t = terms_[i];
                            if (t.hi + 1 > next)
                                continue; // overlapping parts
                            gap(t.hi + 1, next);
                            items.push_back(fit(*phys_.lookup(termKey_[i]), t.hi - t.lo + 1));
                            next = t.lo;
                        }
                        gap(0, next);
                        for (std::size_t j = 0; j < items.size(); ++j)
                            expr += (j ? ", " : "{") + items[j];
                        expr += "}";
                    }
                    else if (whole) {
                        expr = fit(*phys_.lookup(termKey_[*whole]), L);
                    }
                    else if (p.dir == 'i') {
                        const auto d = leaf.defaults.find(p.name);
                        const long long value = d == leaf.defaults.end() ? 0 : d->second;
                        expr = value < 0 ? "{" + std::to_string(L) + "{1'b1}}"
                                         : std::to_string(L) + "'d" + std::to_string(value);
                    }
                    inst << "    ." << p.name << "(" << expr << ")" << (k + 1 < leaf.ports.size() ? ",\n" : "\n");
                }
                inst << "  );\n";
            }
            // hierarchical cells: their ports are the nets that cross them
            for (const auto& k : kids) {
                std::vector<std::string> conns;
                for (const auto& [root, n] : nets)
                    if (n.connected && crosses(n, k)) {
                        // the design port's declared range differs from the child's [w-1:0] only in a scalar
                        conns.push_back("." + n.name + "(" + n.name + ")");
                    }
                inst << "  " << moduleName(k) << " " << ident(k.substr(k.rfind('/') + 1)) << " (";
                for (std::size_t j = 0; j < conns.size(); ++j)
                    inst << (j ? ",\n    " : "\n    ") << conns[j];
                inst << (conns.empty() ? ");\n" : "\n  );\n");
            }
            std::ostringstream m;
            if (mod.empty()) {
                m << "// Generated by OpenCDC from the Vivado block design '" << out_.name
                  << "' (.bd): the cells and their connections.\n"
                  << "module " << out_.name << " (\n";
                for (std::size_t i = 0; i < topPorts_.size(); ++i) {
                    const Port& p = topPorts_[i];
                    m << "  " << (p.dir == 'o' ? "output wire " : p.dir == 'b' ? "inout  wire " : "input  wire ");
                    if (p.range)
                        m << "[" << p.range->first << ":" << p.range->second << "] ";
                    m << p.name << (i + 1 < topPorts_.size() ? ",\n" : "\n");
                }
                m << ");\n" << wires.str() << topAssigns.str() << assigns.str() << inst.str() << "endmodule\n";
            }
            else {
                m << "\n// hierarchical cell '" << mod << "'\nmodule " << moduleName(mod) << " (";
                for (std::size_t j = 0; j < ports.size(); ++j)
                    m << (j ? ",\n  " : "\n  ") << ports[j];
                m << (ports.empty() ? ");\n" : "\n);\n") << wires.str() << assigns.str() << inst.str()
                  << "endmodule\n";
            }
            text += m.str();
        }
        out_.verilog = text;
        if (!multi.empty()) {
            std::string s;
            for (const auto& x : multi)
                s += (s.empty() ? "" : "; ") + x;
            out_.notes.push_back("nets with more than one driver: " + s);
        }
        if (!undriven.empty()) {
            std::string s;
            for (const auto& x : undriven)
                s += (s.empty() ? "" : ", ") + x;
            out_.notes.push_back("nets without a driver (tied to 0): " + s);
        }
    }

    struct NetName {
        int depth;
        std::string name;
        std::string key;
    };

    std::filesystem::path dir_;
    BlockDesign out_;
    std::vector<Port> topPorts_;
    std::map<std::string, std::size_t> topIndex_;
    std::vector<std::string> topInterfaces_;
    std::vector<Leaf> leaves_;
    std::vector<std::string> hierarchies_; ///< paths of the hierarchical cells
    std::vector<Term> terms_;
    std::vector<std::string> termKey_;
    std::map<std::string, std::size_t> termIndex_;
    UnionFind phys_, ifs_;
    std::vector<NetName> netNames_, ifNames_;
    std::map<std::size_t, int> bestIfDepth_;
    std::string missing_;
    std::map<std::string, std::string> ifCase_;
    bool ifCaseBuilt_ = false;
    std::vector<std::string> ifKeys_;
};

} // namespace

bool isBlockDesignFile(const std::filesystem::path& file) {
    return lowerStr(file.extension().string()) == ".bd";
}

Result<BlockDesign> blockDesignFromText(std::string_view text, const std::filesystem::path& dir) {
    json root;
    try {
        root = json::parse(text);
    }
    catch (const std::exception& e) {
        return Diagnostic{"FE-BD-PARSE", Severity::Error,
                          std::string("cannot read the block design (Vivado writes .bd files as JSON since 2014.1): ") +
                              e.what(),
                          {}, {}};
    }
    if (!root.is_object() || !root.contains("design") || !root["design"].is_object())
        return Diagnostic{"FE-BD-PARSE", Severity::Error, "not a Vivado block design (no \"design\" object)", {}, {}};
    Builder b(dir);
    return b.run(root["design"]);
}

Result<BlockDesign> blockDesignFromFile(const std::filesystem::path& file) {
    const auto t = readSourceText(file);
    if (!t)
        return Diagnostic{"FE-BD-PARSE", Severity::Error, "cannot read the block design", SourceLocation{toUtf8(file), 0, 0},
                          {}};
    auto r = blockDesignFromText(t->utf8, file.parent_path());
    if (!r) {
        auto d = std::move(r).error();
        d.location = SourceLocation{toUtf8(file), 0, 0};
        return d;
    }
    return r;
}

} // namespace opencdc::frontend
