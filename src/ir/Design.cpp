#include "ir/Design.h"

#include <algorithm>
#include <cassert>
#include <utility>

namespace opencdc::ir {

std::string_view toString(PortDir d) noexcept {
    switch (d) {
        case PortDir::None: return "none";
        case PortDir::In: return "input";
        case PortDir::Out: return "output";
        case PortDir::InOut: return "inout";
    }
    return "?";
}

std::string_view toString(NetKind k) noexcept {
    switch (k) {
        case NetKind::Wire: return "wire";
        case NetKind::Variable: return "var";
        case NetKind::Temp: return "temp";
    }
    return "?";
}

std::string_view toString(CellKind k) noexcept {
    switch (k) {
        case CellKind::Not: return "not";
        case CellKind::Neg: return "neg";
        case CellKind::ReduceAnd: return "reduce_and";
        case CellKind::ReduceOr: return "reduce_or";
        case CellKind::ReduceXor: return "reduce_xor";
        case CellKind::LogicNot: return "logic_not";
        case CellKind::And: return "and";
        case CellKind::Or: return "or";
        case CellKind::Xor: return "xor";
        case CellKind::Xnor: return "xnor";
        case CellKind::Add: return "add";
        case CellKind::Sub: return "sub";
        case CellKind::Mul: return "mul";
        case CellKind::Div: return "div";
        case CellKind::Mod: return "mod";
        case CellKind::Pow: return "pow";
        case CellKind::Shl: return "shl";
        case CellKind::Shr: return "shr";
        case CellKind::Sshr: return "sshr";
        case CellKind::Eq: return "eq";
        case CellKind::Ne: return "ne";
        case CellKind::EqWild: return "eq_wild";
        case CellKind::Lt: return "lt";
        case CellKind::Le: return "le";
        case CellKind::Gt: return "gt";
        case CellKind::Ge: return "ge";
        case CellKind::LogicAnd: return "logic_and";
        case CellKind::LogicOr: return "logic_or";
        case CellKind::Mux: return "mux";
        case CellKind::DynSelect: return "dyn_select";
        case CellKind::DynInsert: return "dyn_insert";
        case CellKind::Opaque: return "opaque";
    }
    return "?";
}

std::size_t inputCount(CellKind k) noexcept {
    switch (k) {
        case CellKind::Not:
        case CellKind::Neg:
        case CellKind::ReduceAnd:
        case CellKind::ReduceOr:
        case CellKind::ReduceXor:
        case CellKind::LogicNot: return 1;
        case CellKind::Mux:
        case CellKind::DynInsert: return 3;
        case CellKind::Opaque: return 0;
        default: return 2;
    }
}

BitDep bitDependency(CellKind k) noexcept {
    switch (k) {
        case CellKind::Not:
        case CellKind::And:
        case CellKind::Or:
        case CellKind::Xor:
        case CellKind::Xnor:
        case CellKind::Mux: return BitDep::Bitwise;
        default: return BitDep::Full;
    }
}

std::int32_t Net::indexOf(std::uint32_t offset) const noexcept {
    const auto off = static_cast<std::int32_t>(offset);
    return msb >= lsb ? lsb + off : lsb - off;
}

const Attribute* Net::findAttr(std::string_view attrName) const noexcept {
    auto it = std::ranges::find_if(attrs, [&](const Attribute& a) { return a.name == attrName; });
    return it == attrs.end() ? nullptr : &*it;
}

NetId ModuleBody::addNet(Net n) {
    nets.push_back(std::move(n));
    return NetId{static_cast<std::uint32_t>(nets.size() - 1)};
}

NetId ModuleBody::addTemp(std::string tempName, std::uint32_t width) {
    Net n;
    n.name = "$" + tempName + "$" + std::to_string(tempCounter_++);
    n.width = width;
    n.msb = static_cast<std::int32_t>(width) - 1;
    n.lsb = 0;
    n.kind = NetKind::Temp;
    return addNet(std::move(n));
}

SigSpec ModuleBody::addCell(Cell cell) {
    assert(cell.width > 0);
    const NetId out = addTemp(std::string(toString(cell.kind)), cell.width);
    nets.back().loc = cell.origin;
    cell.output = out;
    cells.push_back(std::move(cell));
    return netSig(out);
}

std::optional<NetId> ModuleBody::findNet(std::string_view netName) const {
    for (std::size_t i = 0; i < nets.size(); ++i) {
        if (nets[i].name == netName)
            return NetId{static_cast<std::uint32_t>(i)};
    }
    return std::nullopt;
}

ModuleId Design::addBody(ModuleBody b) {
    bodies.push_back(std::move(b));
    return ModuleId{static_cast<std::uint32_t>(bodies.size() - 1)};
}

InstanceId Design::addInstance(std::string name, std::string hierPath, ModuleId bodyId, InstanceId parent,
                               std::size_t childIndex) {
    InstanceNode node;
    node.name = std::move(name);
    node.hierPath = std::move(hierPath);
    node.body = bodyId;
    node.parent = parent;
    node.childIndex = childIndex;
    const auto& b = body(bodyId);
    node.netBase = nextNet_;
    node.cellBase = nextCell_;
    node.registerBase = nextReg_;
    nextNet_ += b.nets.size();
    nextCell_ += b.cells.size();
    nextReg_ += b.registers.size();
    instances.push_back(std::move(node));
    const InstanceId id{static_cast<std::uint32_t>(instances.size() - 1)};
    if (parent.valid())
        instances[parent.index()].children.push_back(id);
    return id;
}

InstanceId Design::addTop(ModuleId topBody) {
    const auto& tb = body(topBody);
    const InstanceId top = addInstance(tb.name, tb.name, topBody, InstanceId{}, 0);
    tops.push_back(top);

    // Iterative depth-first expansion (deep hierarchies must not overflow the stack).
    std::vector<InstanceId> stack{top};
    while (!stack.empty()) {
        const InstanceId cur = stack.back();
        stack.pop_back();
        const ModuleId curBody = instances[cur.index()].body;
        const std::string curPath = instances[cur.index()].hierPath;
        const auto childCount = body(curBody).children.size();
        std::vector<InstanceId> created;
        for (std::size_t i = 0; i < childCount; ++i) {
            const auto& child = body(curBody).children[i];
            if (!child.body.valid())
                continue; // unknown module: listed in ChildInstance, no subtree
            created.push_back(addInstance(child.name, curPath + "." + child.name, child.body, cur, i));
        }
        for (auto it = created.rbegin(); it != created.rend(); ++it)
            stack.push_back(*it);
    }
    return top;
}

DesignStats Design::stats() const {
    DesignStats s;
    s.uniqueBodies = bodies.size();
    s.instances = instances.size();
    s.unsupported = unsupported.size();
    for (const auto& inst : instances) {
        const auto& b = body(inst.body);
        s.nets += b.nets.size();
        s.cells += b.cells.size();
        s.registers += b.registers.size();
        s.latches += b.latches.size();
        for (const auto& r : b.registers)
            s.registerBits += r.q.width();
    }
    return s;
}

std::optional<InstanceId> Design::findInstance(std::string_view hierPath) const {
    for (std::size_t i = 0; i < instances.size(); ++i) {
        if (instances[i].hierPath == hierPath)
            return InstanceId{static_cast<std::uint32_t>(i)};
    }
    return std::nullopt;
}

} // namespace opencdc::ir
