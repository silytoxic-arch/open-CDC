#include "schematic/Schematic.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace opencdc::schematic {

using namespace opencdc::cdc;

namespace detail {

/// Indexes shared by all schematics of one result.
struct FactoryIndex {
    std::unordered_map<std::string, std::size_t> nodeByName;             ///< source node name -> index
    std::map<std::pair<std::size_t, Pin>, std::vector<std::size_t>> byDestination; ///< (register, pin) -> paths
};

} // namespace detail

const Node* Schematic::find(std::string_view name) const {
    for (const auto& n : nodes)
        if (n.name == name)
            return &n;
    return nullptr;
}

namespace {

/// Last component of a hierarchical name ("top.u_rx.meta" -> "meta").
std::string shortName(std::string_view full) {
    // Escaped identifiers and bit selects may contain dots inside brackets.
    int depth = 0;
    std::size_t cut = 0;
    for (std::size_t i = 0; i < full.size(); ++i) {
        const char c = full[i];
        if (c == '[')
            ++depth;
        else if (c == ']')
            --depth;
        else if (c == '.' && depth == 0)
            cut = i + 1;
    }
    return std::string(full.substr(cut));
}

/// Register name without a trailing bit select ("top.s[1]" -> "top.s").
std::string withoutBit(std::string_view name) {
    if (!name.empty() && name.back() == ']')
        if (auto open = name.rfind('['); open != std::string_view::npos)
            return std::string(name.substr(0, open));
    return std::string(name);
}

std::string_view pinLabel(Pin pin) {
    switch (pin) {
        case Pin::Data: return "";
        case Pin::Enable: return "EN";
        case Pin::SyncReset: return "SRST";
        case Pin::AsyncReset: return "ARST";
    }
    return "";
}

bool isChainType(SyncType t) {
    return t == SyncType::TwoFlop || t == SyncType::ThreeFlop || t == SyncType::MultiFlop || t == SyncType::Toggle ||
           t == SyncType::Gray || t == SyncType::ResetSync;
}

class Builder {
public:
    Builder(const CdcResult& r, const detail::FactoryIndex& index) : r_(r), ix_(index) {}

    Schematic build(std::size_t pathId, const BuildOptions& opt) {
        const CdcPath& focus = r_.paths.at(pathId);
        s_.focusPath = pathId;

        // Paths to draw: the focus path, its structure, and everything that
        // enters the same destination pins (complete merging logic).
        std::vector<std::size_t> todo{pathId};
        if (focus.structure < r_.structures.size())
            for (const auto id : r_.structures[focus.structure].paths)
                todo.push_back(id);
        std::set<std::pair<std::size_t, Pin>> pins;
        for (const auto id : todo)
            pins.emplace(r_.paths[id].dstRegister, r_.paths[id].pin);
        for (const auto& key : pins)
            if (auto it = ix_.byDestination.find(key); it != ix_.byDestination.end())
                for (const auto id : it->second)
                    todo.push_back(id);
        std::vector<std::size_t> chosen;
        for (const auto id : todo)
            if (std::ranges::find(chosen, id) == chosen.end()) {
                if (chosen.size() >= opt.maxPaths) {
                    s_.truncated = true;
                    break;
                }
                chosen.push_back(id);
            }
        s_.paths = chosen;

        for (const auto id : chosen)
            addPath(r_.paths[id]);
        for (const auto id : chosen)
            addReconvergence(r_.paths[id]);

        s_.title = "Path #" + std::to_string(pathId) + ": " + shortName(focus.srcName) + " (" + focus.srcClockName +
                   ") -> " + shortName(focus.dstName) + " (" + focus.dstClockName + ")";
        if (focus.structure < r_.structures.size()) {
            const auto& st = r_.structures[focus.structure];
            if (st.paths.size() > 1)
                s_.title += "  [" + std::string(toString(st.type)) + " structure S" + std::to_string(st.id) + "]";
        }
        return std::move(s_);
    }

private:
    SourceLocation locOf(const std::string& name) const {
        auto it = ix_.nodeByName.find(withoutBit(name));
        return it == ix_.nodeByName.end() ? SourceLocation{} : r_.nodes[it->second].loc;
    }

    std::size_t node(const std::string& name, NodeKind kind, const std::string& clock, std::size_t path,
                     SourceLocation loc = {}) {
        auto it = index_.find(name);
        if (it == index_.end()) {
            Node n;
            n.id = s_.nodes.size();
            n.kind = kind;
            n.name = name;
            n.label = shortName(name);
            n.clock = clock;
            n.loc = loc.file.empty() ? locOf(name) : loc;
            s_.nodes.push_back(std::move(n));
            it = index_.emplace(name, s_.nodes.size() - 1).first;
        }
        Node& n = s_.nodes[it->second];
        if (n.clock.empty())
            n.clock = clock;
        if (std::ranges::find(n.paths, path) == n.paths.end())
            n.paths.push_back(path);
        return it->second;
    }

    void edge(std::size_t from, std::size_t to, EdgeKind kind, const std::string& label, std::size_t path) {
        if (from == to)
            return;
        for (auto& e : s_.edges)
            if (e.from == from && e.to == to && e.kind == kind) {
                if (std::ranges::find(e.paths, path) == e.paths.end())
                    e.paths.push_back(path);
                return;
            }
        Edge e;
        e.from = from;
        e.to = to;
        e.kind = kind;
        e.label = label;
        e.paths.push_back(path);
        const auto& a = s_.nodes[from].clock;
        const auto& b = s_.nodes[to].clock;
        e.crossesClocks = !a.empty() && !b.empty() && a != b;
        s_.edges.push_back(std::move(e));
    }

    static NodeKind kindOfSource(const CdcResult& r, const CdcPath& p) {
        const auto& n = r.nodes.at(p.srcNode);
        if (n.elemWidth > 0)
            return NodeKind::Memory;
        switch (n.kind) {
            case SourceKind::Register: return NodeKind::Register;
            case SourceKind::Latch: return NodeKind::Latch;
            case SourceKind::PrimaryInput: return NodeKind::Input;
            case SourceKind::BlackBox: return NodeKind::BlackBox;
        }
        return NodeKind::Register;
    }

    void addPath(const CdcPath& p) {
        // Source -> logic -> destination, following the reconstructed steps.
        // Nets and ports become edge labels.
        const std::string srcClock = p.srcUnclocked ? std::string() : p.srcClockName;
        std::size_t prev = node(p.srcName, kindOfSource(r_, p), srcClock, p.id, p.srcLoc);
        s_.nodes[prev].roles |= kSource;
        std::string net;
        for (std::size_t i = 1; i < p.steps.size(); ++i) {
            const auto& st = p.steps[i];
            if (st.kind == "net" || st.kind == "port") {
                net = shortName(st.name);
                continue;
            }
            const bool last = i + 1 == p.steps.size();
            if (last)
                break;
            if (st.kind.starts_with("logic:")) {
                const std::size_t n = node(st.name, NodeKind::Logic, "", p.id, st.loc);
                s_.nodes[n].label = st.kind.substr(6);
                edge(prev, n, EdgeKind::Path, net, p.id);
                prev = n;
                net.clear();
            }
        }
        const std::size_t dst = node(p.dstName, NodeKind::Register, p.dstClockName, p.id, p.dstLoc);
        Node& d = s_.nodes[dst];
        d.roles |= kDestination;
        d.status = std::max(d.status, p.status);
        std::string label(pinLabel(p.pin));
        if (label.empty())
            label = net;
        edge(prev, dst, EdgeKind::Path, label, p.id);

        // Synchronizer stages after the first one.
        if (isChainType(p.syncType) && p.syncRegisters.size() > 1) {
            std::size_t stage = dst;
            for (std::size_t k = 1; k < p.syncRegisters.size(); ++k) {
                const std::string& name = p.syncRegisters[k];
                if (withoutBit(name) == p.dstName && withoutBit(p.syncRegisters[k - 1]) == p.dstName)
                    continue; // vector shift register: stages inside the destination register
                const std::size_t n = node(withoutBit(name), NodeKind::Register, p.dstClockName, p.id);
                s_.nodes[n].roles |= kSyncStage;
                edge(stage, n, EdgeKind::Chain, "", p.id);
                stage = n;
            }
            lastStage_[p.id] = stage;
        }
        else {
            lastStage_[p.id] = dst;
        }

        // Synchronized qualifier of captured data.
        const bool qualified = p.syncType == SyncType::QualifiedData || p.syncType == SyncType::Handshake ||
                               p.syncType == SyncType::AsyncFifo;
        if (qualified && !p.syncRegisters.empty()) {
            const std::size_t q = node(p.syncRegisters.front(), NodeKind::Register, p.dstClockName, p.id);
            s_.nodes[q].roles |= kQualifier;
            edge(q, dst, EdgeKind::Qualifier, "EN/SEL", p.id);
        }
    }

    void addReconvergence(const CdcPath& p) {
        for (const auto& rv : p.reconvergences) {
            const std::size_t at = node(rv.at, NodeKind::Register, p.dstClockName, p.id, rv.loc);
            s_.nodes[at].roles |= kReconvergence;
            s_.nodes[at].status = std::max(s_.nodes[at].status, AnalysisStatus::PotentialRisk);
            const std::string label = rv.depth == 0 ? std::string() : "+" + std::to_string(rv.depth) + " reg";
            edge(lastStage_.at(p.id), at, EdgeKind::Reconvergence, label, p.id);
        }
    }

    const CdcResult& r_;
    Schematic s_;
    std::unordered_map<std::string, std::size_t> index_;
    const detail::FactoryIndex& ix_;
    std::unordered_map<std::size_t, std::size_t> lastStage_; ///< path -> its last synchronizer stage node
};

void describe(Schematic& s, const CdcResult& r) {
    for (auto& n : s.nodes) {
        std::string d = n.name;
        switch (n.kind) {
            case NodeKind::Logic: d = "logic " + n.label + "\n" + n.name; break;
            case NodeKind::Memory: d += "\nmemory"; break;
            case NodeKind::Input: d += "\ninput port"; break;
            case NodeKind::BlackBox: d += "\nblack-box output"; break;
            case NodeKind::Latch: d += "\nlatch"; break;
            default: break;
        }
        if (!n.clock.empty())
            d += "\nclock: " + n.clock;
        std::vector<std::string> roles;
        if (n.roles & kSource)
            roles.emplace_back("crossing source");
        if (n.roles & kDestination)
            roles.emplace_back("first synchronizer stage / destination");
        if (n.roles & kSyncStage)
            roles.emplace_back("synchronizer stage");
        if (n.roles & kQualifier)
            roles.emplace_back("synchronized qualifier");
        if (n.roles & kReconvergence)
            roles.emplace_back("reconvergence point");
        for (const auto& role : roles)
            d += "\n" + role;
        if (n.roles & kDestination)
            d += "\nstatus: " + std::string(toString(n.status));
        std::string ids;
        for (const auto id : n.paths)
            ids += (ids.empty() ? "" : " #") + std::to_string(id);
        d += "\npaths: #" + ids;
        if (!n.loc.file.empty())
            d += "\n" + toString(n.loc);
        (void)r;
        n.detail = std::move(d);
    }
}

/// Component boundaries ('.' outside brackets) of a hierarchical name.
std::vector<std::size_t> dots(std::string_view name) {
    std::vector<std::size_t> out;
    int depth = 0;
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '[')
            ++depth;
        else if (name[i] == ']')
            --depth;
        else if (name[i] == '.' && depth == 0)
            out.push_back(i);
    }
    return out;
}

/// Registers with equal short names (u_w2r.s1 and u_r2w.s1) get as many
/// enclosing instance names as needed to tell them apart.
void disambiguate(Schematic& s) {
    for (int round = 0; round < 8; ++round) {
        std::unordered_map<std::string, std::vector<std::size_t>> byLabel;
        for (const auto& n : s.nodes)
            if (n.kind != NodeKind::Logic)
                byLabel[n.label].push_back(n.id);
        bool changed = false;
        for (const auto& [label, ids] : byLabel) {
            if (ids.size() < 2)
                continue;
            for (const auto id : ids) {
                Node& n = s.nodes[id];
                const auto d = dots(n.name);
                // The label covers the last k components; take k + 1.
                const std::size_t k = dots(n.label).size() + 1;
                const std::string longer = k >= d.size() + 1 ? n.name
                                           : k == d.size()   ? n.name
                                                             : n.name.substr(d[d.size() - k - 1] + 1);
                if (longer == n.label)
                    continue;
                n.label = longer;
                changed = true;
            }
        }
        if (!changed)
            break;
    }
}

} // namespace

SchematicFactory::SchematicFactory(const CdcResult& result)
    : result_(result), index_(std::make_unique<detail::FactoryIndex>()) {
    for (std::size_t i = 0; i < result.nodes.size(); ++i)
        index_->nodeByName.emplace(result.nodes[i].name, i);
    for (const auto& p : result.paths)
        index_->byDestination[{p.dstRegister, p.pin}].push_back(p.id);
}

SchematicFactory::~SchematicFactory() = default;

Schematic SchematicFactory::build(std::size_t pathId, const BuildOptions& options) const {
    Builder b(result_, *index_);
    Schematic s = b.build(pathId, options);
    disambiguate(s);
    describe(s, result_);
    return s;
}

Schematic buildForPath(const CdcResult& result, std::size_t pathId, const BuildOptions& options) {
    return SchematicFactory(result).build(pathId, options);
}

} // namespace opencdc::schematic
