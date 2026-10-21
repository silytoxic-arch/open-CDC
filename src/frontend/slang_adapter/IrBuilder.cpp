// Structural part of the slang -> Design IR conversion: declarations,
// continuous assignments, instances, primitives and expression lowering.
// Procedural code is in ProcessLowering.cpp.

#include "frontend/slang_adapter/IrBuilder.h"

#include "ir/ConstFold.h"
#include "ir/Dump.h"

#include <slang/ast/ASTVisitor.h>
#include <slang/ast/EvalContext.h>
#include <slang/ast/expressions/AssignmentExpressions.h>
#include <slang/ast/expressions/CallExpression.h>
#include <slang/ast/expressions/ConversionExpression.h>
#include <slang/ast/expressions/LiteralExpressions.h>
#include <slang/ast/expressions/MiscExpressions.h>
#include <slang/ast/expressions/OperatorExpressions.h>
#include <slang/ast/expressions/SelectExpressions.h>
#include <slang/ast/symbols/AttributeSymbol.h>
#include <slang/ast/symbols/ParameterSymbols.h>
#include <slang/ast/symbols/PortSymbols.h>
#include <slang/ast/symbols/SubroutineSymbols.h>
#include <slang/ast/symbols/VariableSymbols.h>
#include <slang/ast/types/AllTypes.h>
#include <slang/ast/types/Type.h>
#include <slang/numeric/SVInt.h>

#include <algorithm>
#include <map>

namespace opencdc::frontend::detail {

using namespace opencdc::ir;

// ============================================================================
// Helpers
// ============================================================================

SourceLocation convertLocation(const slang::SourceManager& sm, slang::SourceLocation l) {
    if (!l.valid())
        return {};
    l = sm.getFullyExpandedLoc(l);
    if (!l.valid())
        return {};
    SourceLocation out;
    const auto& full = sm.getFullPath(l.buffer());
    if (full.empty()) {
        out.file = std::string(sm.getFileName(l));
    }
    else {
        const auto u8 = full.generic_u8string();
        out.file.assign(u8.begin(), u8.end());
    }
    out.line = static_cast<std::uint32_t>(sm.getLineNumber(l));
    out.column = static_cast<std::uint32_t>(sm.getColumnNumber(l));
    return out;
}

namespace {

ConstBits fromSVInt(const slang::SVInt& v, std::uint32_t width) {
    std::vector<Logic> bits(width, Logic::L0);
    const auto vw = v.getBitWidth();
    for (std::uint32_t i = 0; i < width; ++i) {
        if (i >= vw) {
            bits[i] = (v.isSigned() && vw > 0 && v[static_cast<std::int32_t>(vw - 1)].value == 1) ? Logic::L1
                                                                                                 : Logic::L0;
            continue;
        }
        const slang::logic_t b = v[static_cast<std::int32_t>(i)];
        if (b.value == slang::logic_t::z.value)
            bits[i] = Logic::Z;
        else if (b.isUnknown())
            bits[i] = Logic::X;
        else
            bits[i] = b.value ? Logic::L1 : Logic::L0;
    }
    return ConstBits(std::move(bits));
}

std::uint32_t typeWidth(const slang::ast::Type& t) {
    if (t.isIntegral())
        return t.getBitWidth();
    if (t.isFixedSize() && t.isUnpackedArray())
        return static_cast<std::uint32_t>(t.getBitstreamWidth());
    return 0;
}

std::uint32_t exprWidth(const slang::ast::Expression& e) {
    return typeWidth(*e.type);
}

PortDir convertDir(slang::ast::ArgumentDirection d) {
    switch (d) {
        case slang::ast::ArgumentDirection::In: return PortDir::In;
        case slang::ast::ArgumentDirection::Out: return PortDir::Out;
        case slang::ast::ArgumentDirection::InOut:
        case slang::ast::ArgumentDirection::Ref: return PortDir::InOut;
    }
    return PortDir::None;
}

/// Index range and element width of something that can be selected from.
struct SelectInfo {
    std::int32_t left = 0;
    std::int32_t right = 0;
    std::uint32_t elemWidth = 1;
};

std::optional<SelectInfo> selectInfo(const slang::ast::Type& type) {
    const auto& t = type.getCanonicalType();
    if (t.isPackedArray() || (t.isUnpackedArray() && t.isFixedSize())) {
        auto r = t.getFixedRange();
        const auto* elem = t.getArrayElementType();
        if (!elem)
            return std::nullopt;
        const std::uint32_t ew = typeWidth(*elem);
        if (ew == 0)
            return std::nullopt;
        return SelectInfo{r.left, r.right, ew};
    }
    if (t.isIntegral()) {
        return SelectInfo{static_cast<std::int32_t>(t.getBitWidth()) - 1, 0, 1};
    }
    return std::nullopt;
}

/// Verilog string literals are integers; show attribute values written as
/// strings ("TRUE") as strings again.
std::string attributeText(const slang::ConstantValue& v) {
    if (v.isString())
        return "\"" + v.str() + "\"";
    if (v.isInteger()) {
        const auto& i = v.integer();
        const auto w = i.getBitWidth();
        if (w >= 8 && w % 8 == 0 && !i.hasUnknown()) {
            std::string text;
            bool printable = true;
            for (std::uint32_t byte = w / 8; byte-- > 0 && printable;) {
                unsigned c = 0;
                for (std::uint32_t b = 0; b < 8; ++b)
                    if (i[static_cast<std::int32_t>(byte * 8 + b)].value == 1)
                        c |= 1U << b;
                if (c == 0 && text.empty())
                    continue; // leading zero padding
                printable = c >= 0x20 && c < 0x7f;
                text += static_cast<char>(c);
            }
            if (printable && !text.empty())
                return "\"" + text + "\"";
        }
    }
    return v.toString();
}

std::optional<std::int64_t> constIndex(const SigSpec& s, bool isSigned) {
    auto c = s.asConst();
    if (!c || c->width() == 0 || c->width() > 64)
        return std::nullopt;
    if (isSigned)
        return c->toInt();
    auto u = c->toUint();
    if (!u || *u > static_cast<std::uint64_t>(INT64_MAX))
        return std::nullopt;
    return static_cast<std::int64_t>(*u);
}

} // namespace

// ============================================================================
// IrBuilder
// ============================================================================

IrBuilder::IrBuilder(const slang::SourceManager& sm, DiagnosticSink& sink) : sm_(sm), sink_(sink) {}

SourceLocation IrBuilder::loc(slang::SourceLocation l) const {
    return convertLocation(sm_, l);
}

std::shared_ptr<Design> IrBuilder::build(const sast::RootSymbol& root) {
    design_ = std::make_shared<Design>();
    for (const auto* top : root.topInstances) {
        const ModuleId id = bodyFor(*top);
        if (id.valid())
            design_->addTop(id);
    }
    return design_;
}

ModuleId IrBuilder::bodyFor(const sast::InstanceSymbol& inst) {
    const sast::InstanceBodySymbol* b = inst.getCanonicalBody();
    if (!b)
        b = &inst.body;
    if (auto it = built_.find(b); it != built_.end())
        return it->second;

    if (++depth_ > 512) {
        --depth_;
        report({"IR-UNSUPPORTED-DEPTH", Severity::Error, "instance hierarchy too deep (> 512 levels)",
                loc(inst.location), {}});
        return {};
    }
    BodyBuilder builder(*this, *b);
    ModuleBody mb = builder.build();
    --depth_;
    const ModuleId id = design_->addBody(std::move(mb));
    netMaps_.push_back(builder.takeNetMap());
    built_.emplace(b, id);
    return id;
}

// ============================================================================
// BodyBuilder: declarations and structure
// ============================================================================

BodyBuilder::BodyBuilder(IrBuilder& owner, const sast::InstanceBodySymbol& body) : owner_(owner), sym_(body) {
    if (body.parentInstance)
        prefix_ = body.parentInstance->getHierarchicalPath() + ".";
}

SourceLocation BodyBuilder::loc(slang::SourceLocation l) const {
    return owner_.loc(l);
}

void BodyBuilder::unsupported(const std::string& code, const std::string& what, slang::SourceLocation l) {
    const auto where = loc(l);
    owner_.design().unsupported.push_back({code, body_.name, what, where});
    owner_.report({code, Severity::Warning, what + " (not analyzed; modelled conservatively where possible)", where, {}});
}

void BodyBuilder::warn(const std::string& code, Severity sev, const std::string& msg, slang::SourceLocation l) {
    owner_.report({code, sev, msg, loc(l), {}});
}

std::string BodyBuilder::relativeName(const sast::Symbol& s) const {
    std::string path = s.getHierarchicalPath();
    if (!prefix_.empty() && path.starts_with(prefix_))
        return path.substr(prefix_.size());
    return std::string(s.name);
}

ModuleBody BodyBuilder::build() {
    body_.name = std::string(sym_.getDefinition().name);
    body_.loc = loc(sym_.getDefinition().location);

    std::string params;
    for (const auto& m : sym_.members()) {
        if (m.kind != sast::SymbolKind::Parameter)
            continue;
        const auto& p = m.as<sast::ParameterSymbol>();
        if (!p.isPortParam())
            continue;
        if (!params.empty())
            params += ", ";
        params += std::string(p.name) + "=" + p.getValue().toString();
    }
    if (!params.empty())
        body_.params = "#(" + params + ")";

    declarePorts();
    declareScope(sym_);
    lowerScope(sym_);
    pruneDeadStorage();
    removeDeadCells();
    checkMultipleDrivers();
    return std::move(body_);
}

bool BodyBuilder::isModuleLevelStorage(const sast::ValueSymbol& s) const {
    if (s.kind == sast::SymbolKind::Variable) {
        if (s.as<sast::VariableSymbol>().lifetime == sast::VariableLifetime::Automatic)
            return false;
    }
    else if (s.kind != sast::SymbolKind::Net) {
        return false;
    }
    // Function/task locals are never storage of the module.
    for (const sast::Scope* sc = s.getParentScope(); sc; sc = sc->asSymbol().getParentScope()) {
        const auto& owner = sc->asSymbol();
        if (owner.kind == sast::SymbolKind::Subroutine)
            return false;
        if (&owner == &sym_)
            return true;
    }
    return true;
}

std::uint32_t BodyBuilder::widthOf(const sast::ValueSymbol& s) const {
    if (auto it = netOf_.find(&s); it != netOf_.end())
        return body_.net(it->second).width;
    return typeWidth(s.getType());
}

std::optional<NetId> BodyBuilder::netFor(const sast::ValueSymbol& s) {
    if (auto it = netOf_.find(&s); it != netOf_.end())
        return it->second;
    if (!isModuleLevelStorage(s))
        return std::nullopt;

    const auto& t = s.getType().getCanonicalType();
    Net n;
    n.name = relativeName(s);
    n.kind = s.kind == sast::SymbolKind::Net ? NetKind::Wire : NetKind::Variable;
    n.loc = loc(s.location);
    if (t.isIntegral()) {
        n.width = t.getBitWidth();
        n.isSigned = t.isSigned();
        n.msb = static_cast<std::int32_t>(n.width) - 1;
        n.lsb = 0;
        if (t.isPackedArray()) {
            const auto* elem = t.getArrayElementType();
            if (elem && elem->getBitWidth() == 1) {
                auto r = t.getFixedRange();
                n.msb = r.left;
                n.lsb = r.right;
            }
        }
    }
    else if (t.isUnpackedArray() && t.isFixedSize()) {
        const auto* elem = t.getArrayElementType();
        if (!elem || !elem->isIntegral()) {
            unsupported("IR-UNSUPPORTED-TYPE",
                        "array '" + std::string(s.name) + "' with non-integral or multi-dimensional elements",
                        s.location);
            return std::nullopt;
        }
        auto r = t.getFixedRange();
        n.elemWidth = elem->getBitWidth();
        n.arrayDepth = r.width();
        n.width = n.elemWidth * n.arrayDepth;
        n.msb = r.left;
        n.lsb = r.right;
    }
    else {
        unsupported("IR-UNSUPPORTED-TYPE",
                    "signal '" + std::string(s.name) + "' of type '" + t.toString() + "'", s.location);
        return std::nullopt;
    }
    if (n.width == 0)
        return std::nullopt;

    if (const auto* scope = s.getParentScope()) {
        for (const auto* attr : scope->getCompilation().getAttributes(s))
            n.attrs.push_back({std::string(attr->name), attributeText(attr->getValue())});
    }

    const NetId id = body_.addNet(std::move(n));
    netOf_.emplace(&s, id);
    return id;
}

void BodyBuilder::declarePorts() {
    for (const auto* p : sym_.getPortList()) {
        if (p->kind != sast::SymbolKind::Port) {
            unsupported("IR-UNSUPPORTED-PORT", "port '" + std::string(p->name) + "' (interface or multi-port)",
                        p->location);
            continue;
        }
        const auto& ps = p->as<sast::PortSymbol>();
        if (!ps.internalSymbol || !ps.internalSymbol->isValue()) {
            if (!ps.isNullPort)
                unsupported("IR-UNSUPPORTED-PORT", "port '" + std::string(ps.name) + "' with explicit port expression",
                            ps.location);
            continue;
        }
        auto net = netFor(ps.internalSymbol->as<sast::ValueSymbol>());
        if (!net)
            continue;
        body_.net(*net).dir = convertDir(ps.direction);
        body_.ports.push_back({std::string(ps.name), convertDir(ps.direction), *net});
    }
}

void BodyBuilder::declareScope(const sast::Scope& scope) {
    for (const auto& m : scope.members()) {
        switch (m.kind) {
            case sast::SymbolKind::Net:
            case sast::SymbolKind::Variable: (void)netFor(m.as<sast::ValueSymbol>()); break;
            case sast::SymbolKind::GenerateBlock:
                if (!m.as<sast::GenerateBlockSymbol>().isUninstantiated)
                    declareScope(m.as<sast::GenerateBlockSymbol>());
                break;
            case sast::SymbolKind::GenerateBlockArray:
                for (const auto* e : m.as<sast::GenerateBlockArraySymbol>().entries)
                    if (!e->isUninstantiated)
                        declareScope(*e);
                break;
            default: break;
        }
    }
}

void BodyBuilder::lowerScope(const sast::Scope& scope) {
    for (const auto& m : scope.members())
        lowerMember(m);
}

void BodyBuilder::lowerMember(const sast::Symbol& m) {
    switch (m.kind) {
        case sast::SymbolKind::ContinuousAssign: lowerContinuousAssign(m.as<sast::ContinuousAssignSymbol>()); break;
        case sast::SymbolKind::Net:
        case sast::SymbolKind::Variable:
            if (m.as<sast::ValueSymbol>().getInitializer())
                lowerNetInitializer(m.as<sast::ValueSymbol>());
            break;
        case sast::SymbolKind::ProceduralBlock: lowerProcedure(m.as<sast::ProceduralBlockSymbol>()); break;
        case sast::SymbolKind::Instance: lowerInstance(m.as<sast::InstanceSymbol>()); break;
        case sast::SymbolKind::InstanceArray:
            for (const auto* e : m.as<sast::InstanceArraySymbol>().elements)
                lowerMember(*e);
            break;
        case sast::SymbolKind::PrimitiveInstance: lowerPrimitive(m.as<sast::PrimitiveInstanceSymbol>()); break;
        case sast::SymbolKind::GenerateBlock:
            if (!m.as<sast::GenerateBlockSymbol>().isUninstantiated)
                lowerScope(m.as<sast::GenerateBlockSymbol>());
            break;
        case sast::SymbolKind::GenerateBlockArray:
            for (const auto* e : m.as<sast::GenerateBlockArraySymbol>().entries)
                lowerMember(*e);
            break;
        case sast::SymbolKind::UninstantiatedDef: {
            ChildInstance ci;
            ci.name = relativeName(m);
            ci.definition = "?";
            ci.loc = loc(m.location);
            body_.children.push_back(std::move(ci));
            owner_.design().unsupported.push_back(
                {"IR-UNRESOLVED-INSTANCE", body_.name, "instance '" + std::string(m.name) + "' of an unknown module",
                 loc(m.location)});
            break;
        }
        default: break; // parameters, genvars, functions, typedefs, ports ... carry no logic
    }
}

void BodyBuilder::lowerContinuousAssign(const sast::ContinuousAssignSymbol& ca) {
    const auto& e = ca.getAssignment();
    if (e.kind != sast::ExpressionKind::Assignment)
        return; // slang reported the error already
    const auto& a = e.as<sast::AssignmentExpression>();
    auto targets = lvalue(a.left());
    SigSpec rhs = lower(a.right());
    if (targets)
        assignTargets(*targets, rhs, false, ca.location);
}

void BodyBuilder::lowerNetInitializer(const sast::ValueSymbol& s) {
    const auto* init = s.getInitializer();
    auto net = netFor(s);
    if (!init || !net)
        return;
    SigSpec value = lower(*init).resized(body_.net(*net).width, false);
    if (s.kind == sast::SymbolKind::Net) {
        body_.connections.push_back({body_.netSig(*net), value, ConnKind::Assign, loc(s.location)});
    }
    else if (auto c = value.asConst()) {
        body_.net(*net).init = *c; // variable initializer = power-up value
    }
    else {
        unsupported("IR-UNSUPPORTED-INIT", "non-constant initializer of variable '" + std::string(s.name) + "'",
                    s.location);
    }
}

void BodyBuilder::lowerInstance(const sast::InstanceSymbol& inst) {
    if (!inst.isModule()) {
        unsupported("IR-UNSUPPORTED-INSTANCE",
                    "instance '" + std::string(inst.name) + "' of interface/program '" +
                        std::string(inst.getDefinition().name) + "'",
                    inst.location);
        return;
    }
    const ModuleId cid = owner_.bodyFor(inst);
    ChildInstance ci;
    ci.name = relativeName(inst);
    ci.definition = std::string(inst.getDefinition().name);
    ci.body = cid;
    ci.loc = loc(inst.location);

    for (const auto* pc : inst.getPortConnections()) {
        if (pc->port.kind != sast::SymbolKind::Port) {
            unsupported("IR-UNSUPPORTED-PORT", "interface port connection on '" + ci.name + "'", inst.location);
            continue;
        }
        const auto& ps = pc->port.as<sast::PortSymbol>();
        PortBinding pb;
        pb.port = std::string(ps.name);
        pb.dir = convertDir(ps.direction);
        if (cid.valid()) {
            for (const auto& p : owner_.design().body(cid).ports) {
                if (p.name == pb.port) {
                    pb.childNet = p.net;
                    break;
                }
            }
        }
        const auto* expr = pc->getExpression();
        if (expr && expr->kind != sast::ExpressionKind::EmptyArgument) {
            if (pb.dir == PortDir::In) {
                pb.actual = lower(*expr);
            }
            else {
                const sast::Expression* lv = expr;
                if (expr->kind == sast::ExpressionKind::Assignment &&
                    expr->as<sast::AssignmentExpression>().isLValueArg())
                    lv = &expr->as<sast::AssignmentExpression>().left();
                if (auto targets = lvalue(*lv)) {
                    for (const auto& t : *targets) {
                        auto n = t.dynamic ? std::nullopt : netFor(*t.sym);
                        if (!n) {
                            unsupported("IR-UNSUPPORTED-PORT",
                                        "output port '" + pb.port + "' of '" + ci.name +
                                            "' connected to an unsupported expression",
                                        expr->sourceRange.start());
                            pb.actual = {};
                            break;
                        }
                        pb.actual.append(SigSpec::net(*n, t.width, t.offset));
                    }
                }
            }
        }
        ci.ports.push_back(std::move(pb));
    }
    body_.children.push_back(std::move(ci));
}

void BodyBuilder::lowerPrimitive(const sast::PrimitiveInstanceSymbol& prim) {
    const std::string kind(prim.primitiveType.name);
    auto conns = prim.getPortConnections();
    auto outTargets = [&](const sast::Expression* e) -> std::optional<std::vector<LTarget>> {
        if (!e)
            return std::nullopt;
        if (e->kind == sast::ExpressionKind::Assignment && e->as<sast::AssignmentExpression>().isLValueArg())
            e = &e->as<sast::AssignmentExpression>().left();
        return lvalue(*e);
    };

    static const std::map<std::string, std::pair<CellKind, bool>> nInput = {
        {"and", {CellKind::And, false}},  {"nand", {CellKind::And, true}}, {"or", {CellKind::Or, false}},
        {"nor", {CellKind::Or, true}},    {"xor", {CellKind::Xor, false}}, {"xnor", {CellKind::Xor, true}},
    };
    if (auto it = nInput.find(kind); it != nInput.end() && conns.size() >= 2) {
        SigSpec acc = lower(*conns[1]).resized(1, false);
        for (std::size_t i = 2; i < conns.size(); ++i)
            acc = cell(it->second.first, {acc, lower(*conns[i]).resized(1, false)}, 1, prim.location);
        if (it->second.second)
            acc = cell(CellKind::Not, {acc}, 1, prim.location);
        if (auto t = outTargets(conns[0]))
            assignTargets(*t, acc.resized(static_cast<std::uint32_t>(
                                               std::max<std::size_t>(1, t->empty() ? 1 : (*t)[0].width)), false),
                          false, prim.location);
        return;
    }
    if ((kind == "buf" || kind == "not") && conns.size() >= 2) {
        SigSpec in = lower(*conns.back()).resized(1, false);
        if (kind == "not")
            in = cell(CellKind::Not, {in}, 1, prim.location);
        for (std::size_t i = 0; i + 1 < conns.size(); ++i) {
            if (auto t = outTargets(conns[i]))
                assignTargets(*t, in, false, prim.location);
        }
        return;
    }
    unsupported("IR-UNSUPPORTED-PRIMITIVE", "primitive/UDP '" + kind + "' instance '" + std::string(prim.name) + "'",
                prim.location);
}

// ============================================================================
// Driver checks and cleanup
// ============================================================================

void BodyBuilder::pruneDeadStorage() {
    // Registers and latches whose output cannot reach anything observable
    // (ports, continuous assignments, child inputs, or other live storage)
    // are not hardware: temporaries of always blocks, loop indices, unused
    // state. Synthesis removes them; so do we. Liveness is propagated
    // backwards through combinational cells (a storage element feeding only
    // its own hold multiplexer stays dead).
    const std::size_t nNets = body_.nets.size();
    std::vector<std::int64_t> cellOf(nNets, -1);
    for (std::size_t i = 0; i < body_.cells.size(); ++i)
        cellOf[body_.cells[i].output.index()] = static_cast<std::int64_t>(i);

    std::vector<std::vector<bool>> reached(nNets);
    std::vector<bool> cellDone(body_.cells.size(), false);
    std::vector<SigSpec> work;
    auto visit = [&](const SigSpec& sig) { work.push_back(sig); };
    auto drain = [&] {
        while (!work.empty()) {
            SigSpec sig = std::move(work.back());
            work.pop_back();
            for (const auto& c : sig.chunks()) {
                if (c.isConst())
                    continue;
                auto& m = reached[c.net.index()];
                if (m.empty())
                    m.resize(body_.nets[c.net.index()].width, false);
                for (std::uint32_t i = c.offset; i < c.offset + c.width && i < m.size(); ++i)
                    m[i] = true;
                const auto ci = cellOf[c.net.index()];
                if (ci >= 0 && !cellDone[static_cast<std::size_t>(ci)]) {
                    cellDone[static_cast<std::size_t>(ci)] = true;
                    for (const auto& in : body_.cells[static_cast<std::size_t>(ci)].inputs)
                        work.push_back(in);
                }
            }
        }
    };
    auto isReached = [&](const SigSpec& q) {
        for (const auto& c : q.chunks()) {
            if (c.isConst())
                continue;
            if (body_.nets[c.net.index()].dir != PortDir::None)
                return true;
            const auto& m = reached[c.net.index()];
            for (std::uint32_t i = c.offset; i < c.offset + c.width && i < m.size(); ++i)
                if (m[i])
                    return true;
        }
        return false;
    };

    for (const auto& c : body_.connections)
        visit(c.rhs);
    for (const auto& ch : body_.children)
        for (const auto& p : ch.ports)
            if (p.dir != PortDir::Out)
                visit(p.actual);
    drain();

    std::vector<bool> regLive(body_.registers.size(), false);
    std::vector<bool> latchLive(body_.latches.size(), false);
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t i = 0; i < body_.registers.size(); ++i) {
            const auto& r = body_.registers[i];
            if (regLive[i] || !isReached(r.q))
                continue;
            regLive[i] = changed = true;
            visit(r.d);
            visit(r.clock.signal);
            for (const auto& a : r.asyncResets) {
                visit(a.signal);
                visit(a.value);
            }
            if (r.syncReset) {
                visit(r.syncReset->signal);
                visit(r.syncReset->value);
            }
            if (r.enable)
                visit(*r.enable);
        }
        for (std::size_t i = 0; i < body_.latches.size(); ++i) {
            if (latchLive[i] || !isReached(body_.latches[i].q))
                continue;
            latchLive[i] = changed = true;
            visit(body_.latches[i].d);
        }
        drain();
    }

    std::size_t removedRegs = 0;
    std::size_t removedLatches = 0;
    std::vector<Register> regs;
    for (std::size_t i = 0; i < body_.registers.size(); ++i) {
        if (regLive[i])
            regs.push_back(std::move(body_.registers[i]));
        else
            ++removedRegs;
    }
    std::vector<Latch> latches;
    std::vector<bool> flags;
    for (std::size_t i = 0; i < body_.latches.size(); ++i) {
        if (latchLive[i]) {
            latches.push_back(std::move(body_.latches[i]));
            flags.push_back(i < latchUnintended_.size() && latchUnintended_[i]);
        }
        else {
            ++removedLatches;
        }
    }
    body_.registers = std::move(regs);
    body_.latches = std::move(latches);
    latchUnintended_ = std::move(flags);

    if (removedRegs + removedLatches > 0) {
        owner_.report({"IR-DEAD-STORAGE", Severity::Info,
                       "module '" + body_.name + "': removed " + std::to_string(removedRegs) + " register(s) and " +
                           std::to_string(removedLatches) + " latch(es) whose outputs are never read",
                       body_.loc, {}});
    }
    for (std::size_t i = 0; i < body_.latches.size(); ++i) {
        if (i < latchUnintended_.size() && latchUnintended_[i]) {
            owner_.report({"IR-LATCH-INFERRED", Severity::Warning,
                           "latch inferred for '" + formatSig(body_, body_.latches[i].q) + "' in module '" + body_.name +
                               "': not assigned on every path of a combinational block",
                           body_.latches[i].origin, {}});
        }
    }
}

void BodyBuilder::removeDeadCells() {
    // Live cells are those reachable (backwards) from anything observable:
    // connections, registers, latches and child port bindings.
    const std::size_t nNets = body_.nets.size();
    std::vector<std::int64_t> cellOf(nNets, -1);
    for (std::size_t i = 0; i < body_.cells.size(); ++i)
        cellOf[body_.cells[i].output.index()] = static_cast<std::int64_t>(i);

    std::vector<bool> live(body_.cells.size(), false);
    std::vector<std::size_t> work;
    auto use = [&](const SigSpec& s) {
        for (const auto& c : s.chunks()) {
            if (c.isConst())
                continue;
            const auto ci = cellOf[c.net.index()];
            if (ci >= 0 && !live[static_cast<std::size_t>(ci)]) {
                live[static_cast<std::size_t>(ci)] = true;
                work.push_back(static_cast<std::size_t>(ci));
            }
        }
    };
    for (const auto& c : body_.connections) {
        use(c.lhs);
        use(c.rhs);
    }
    for (const auto& r : body_.registers) {
        use(r.d);
        use(r.clock.signal);
        for (const auto& a : r.asyncResets) {
            use(a.signal);
            use(a.value);
        }
        if (r.syncReset) {
            use(r.syncReset->signal);
            use(r.syncReset->value);
        }
        if (r.enable)
            use(*r.enable);
    }
    for (const auto& l : body_.latches)
        use(l.d);
    for (const auto& ch : body_.children)
        for (const auto& p : ch.ports)
            use(p.actual);
    while (!work.empty()) {
        const auto ci = work.back();
        work.pop_back();
        for (const auto& in : body_.cells[ci].inputs)
            use(in);
    }
    if (std::ranges::all_of(live, [](bool b) { return b; }))
        return;

    // Compact: drop dead cells and their temp nets, renumber everything.
    std::vector<bool> keepNet(nNets, true);
    for (std::size_t i = 0; i < body_.cells.size(); ++i)
        if (!live[i])
            keepNet[body_.cells[i].output.index()] = false;
    std::vector<std::uint32_t> remap(nNets, 0);
    std::vector<Net> nets;
    for (std::size_t i = 0; i < nNets; ++i) {
        if (!keepNet[i])
            continue;
        remap[i] = static_cast<std::uint32_t>(nets.size());
        nets.push_back(std::move(body_.nets[i]));
    }
    auto fix = [&](SigSpec& s) {
        SigSpec out;
        for (auto c : s.chunks()) {
            if (!c.isConst())
                c.net = NetId{remap[c.net.index()]};
            out.append(c);
        }
        s = std::move(out);
    };
    std::vector<Cell> cells;
    for (std::size_t i = 0; i < body_.cells.size(); ++i) {
        if (!live[i])
            continue;
        Cell c = std::move(body_.cells[i]);
        for (auto& in : c.inputs)
            fix(in);
        c.output = NetId{remap[c.output.index()]};
        cells.push_back(std::move(c));
    }
    for (auto& c : body_.connections) {
        fix(c.lhs);
        fix(c.rhs);
    }
    for (auto& r : body_.registers) {
        fix(r.q);
        fix(r.d);
        fix(r.clock.signal);
        for (auto& a : r.asyncResets) {
            fix(a.signal);
            fix(a.value);
        }
        if (r.syncReset) {
            fix(r.syncReset->signal);
            fix(r.syncReset->value);
        }
        if (r.enable)
            fix(*r.enable);
    }
    for (auto& l : body_.latches) {
        fix(l.q);
        fix(l.d);
    }
    for (auto& ch : body_.children)
        for (auto& p : ch.ports)
            fix(p.actual);
    for (auto& p : body_.ports)
        p.net = NetId{remap[p.net.index()]};
    for (auto& [sym, id] : netOf_)
        id = NetId{remap[id.index()]};
    body_.nets = std::move(nets);
    body_.cells = std::move(cells);
    driverCell_.clear();
    cellCache_.clear();
}

void BodyBuilder::checkMultipleDrivers() {
    struct Drv {
        std::uint32_t lo;
        std::uint32_t hi;
        std::string what;
    };
    std::vector<std::vector<Drv>> drivers(body_.nets.size());
    auto add = [&](const SigSpec& s, const std::string& what) {
        for (const auto& c : s.chunks())
            if (!c.isConst())
                drivers[c.net.index()].push_back({c.offset, c.offset + c.width, what});
    };
    for (const auto& p : body_.ports)
        if (p.dir == PortDir::In)
            add(body_.netSig(p.net), "input port");
    for (const auto& c : body_.connections)
        add(c.lhs, (c.kind == ConnKind::Process ? "always block at " : "assignment at ") + toString(c.origin));
    for (const auto& r : body_.registers)
        add(r.q, "always block at " + toString(r.origin));
    for (const auto& l : body_.latches)
        add(l.q, "always block at " + toString(l.origin));
    for (const auto& ch : body_.children)
        for (const auto& p : ch.ports)
            if (p.dir == PortDir::Out)
                add(p.actual, "output port ." + p.port + " of " + ch.name);

    for (std::size_t n = 0; n < drivers.size(); ++n) {
        auto& ds = drivers[n];
        if (ds.size() < 2)
            continue;
        std::ranges::sort(ds, {}, &Drv::lo);
        for (std::size_t i = 0; i + 1 < ds.size(); ++i) {
            for (std::size_t j = i + 1; j < ds.size() && ds[j].lo < ds[i].hi; ++j) {
                // Several assignments from the same always block are one driver.
                if (ds[i].what == ds[j].what)
                    continue;
                const auto& net = body_.nets[n];
                const std::uint32_t bit = ds[j].lo;
                owner_.report({"IR-MULTIDRIVER", Severity::Error,
                               "'" + net.name + "' bit [" + std::to_string(net.indexOf(bit)) + "] in module '" +
                                   body_.name + "' has multiple drivers: " + ds[i].what + " and " + ds[j].what,
                               net.loc, {}});
                goto nextNet; // one diagnostic per net is enough
            }
        }
    nextNet:;
    }
}

// ============================================================================
// Expressions
// ============================================================================

namespace {

void appendKey(std::string& key, const SigSpec& s) {
    for (const auto& c : s.chunks()) {
        if (c.isConst())
            key += c.constant.toString();
        else
            key += "n" + std::to_string(c.net.value) + "." + std::to_string(c.offset) + "." + std::to_string(c.width);
        key += ',';
    }
    key += '|';
}

} // namespace

SigSpec BodyBuilder::emit(Cell c) {
    if (auto simplified = simplifyCell(c))
        return simplified->resized(c.width, false);
    // Structural hashing: an identical cell already exists -> reuse its output.
    std::string key = std::to_string(static_cast<int>(c.kind)) + ":" + std::to_string(c.width) + ":" +
                      (c.isSigned ? "s" : "u") + ":" + std::to_string(c.elemWidth) + ":" +
                      std::to_string(c.rangeLeft) + ":" + std::to_string(c.rangeRight) + ":";
    for (const auto& in : c.inputs)
        appendKey(key, in);
    if (auto it = cellCache_.find(key); it != cellCache_.end())
        return it->second;
    SigSpec out = body_.addCell(std::move(c));
    driverCell_[body_.cells.back().output.value] = body_.cells.size() - 1;
    cellCache_.emplace(std::move(key), out);
    return out;
}

SigSpec BodyBuilder::cell(CellKind kind, std::vector<SigSpec> inputs, std::uint32_t width, slang::SourceLocation l,
                          bool isSigned) {
    Cell c;
    c.kind = kind;
    c.inputs = std::move(inputs);
    c.width = width;
    c.isSigned = isSigned;
    c.origin = loc(l);
    return emit(std::move(c));
}

SigSpec BodyBuilder::toBool(const SigSpec& s, slang::SourceLocation l) {
    if (s.width() == 1)
        return s;
    if (s.width() == 0)
        return SigSpec::constant(0, 1);
    return cell(CellKind::ReduceOr, {s}, 1, l);
}

SigSpec BodyBuilder::opaque(const sast::Expression& e, const std::string& what) {
    const std::uint32_t w = exprWidth(e);
    unsupported("IR-UNSUPPORTED-EXPR", what, e.sourceRange.start());
    if (w == 0)
        return {};
    // Conservatively keep connectivity: the result depends on every signal read.
    std::vector<SigSpec> inputs;
    std::unordered_set<const sast::ValueSymbol*> seen;
    auto visitor = sast::makeVisitor([&](auto&, const sast::NamedValueExpression& nv) {
        if (seen.insert(&nv.symbol).second &&
            (nv.symbol.kind == sast::SymbolKind::Net || nv.symbol.kind == sast::SymbolKind::Variable ||
             nv.symbol.kind == sast::SymbolKind::FormalArgument)) {
            SigSpec v = read(nv.symbol);
            if (!v.empty())
                inputs.push_back(std::move(v));
        }
    });
    e.visit(visitor);
    Cell c;
    c.kind = CellKind::Opaque;
    c.inputs = std::move(inputs);
    c.width = w;
    c.label = what;
    c.origin = loc(e.sourceRange.start());
    SigSpec out = body_.addCell(std::move(c));
    driverCell_[body_.cells.back().output.value] = body_.cells.size() - 1;
    return out;
}

std::optional<SigSpec> BodyBuilder::tryConstant(const sast::Expression& e) {
    const std::uint32_t w = exprWidth(e);
    if (w == 0)
        return std::nullopt;
    if (const auto* cv = e.getConstant(); cv && cv->isInteger())
        return SigSpec::constant(fromSVInt(cv->integer(), w));
    return std::nullopt;
}

SigSpec BodyBuilder::lower(const sast::Expression& e) {
    const std::uint32_t w = exprWidth(e);
    if (auto c = tryConstant(e))
        return *c;
    SigSpec s = lowerImpl(e);
    if (s.width() != w && w > 0 && !s.empty())
        s = s.resized(w, e.type->isSigned());
    return s;
}

SigSpec BodyBuilder::lowerImpl(const sast::Expression& e) {
    using EK = sast::ExpressionKind;
    const std::uint32_t w = exprWidth(e);
    switch (e.kind) {
        case EK::IntegerLiteral:
            return SigSpec::constant(fromSVInt(e.as<sast::IntegerLiteral>().getValue(), w));
        case EK::UnbasedUnsizedIntegerLiteral:
            return SigSpec::constant(fromSVInt(e.as<sast::UnbasedUnsizedIntegerLiteral>().getValue(), w));
        case EK::NamedValue: {
            const auto& s = e.as<sast::NamedValueExpression>().symbol;
            switch (s.kind) {
                case sast::SymbolKind::Net:
                case sast::SymbolKind::Variable:
                case sast::SymbolKind::FormalArgument: return read(s);
                default: {
                    // parameters, enum values, genvars: constant-evaluate
                    sast::EvalContext ctx(sym_);
                    auto cv = e.eval(ctx);
                    if (cv.isInteger())
                        return SigSpec::constant(fromSVInt(cv.integer(), w));
                    return opaque(e, "reference to '" + std::string(s.name) + "'");
                }
            }
        }
        case EK::HierarchicalValue:
            return opaque(e, "hierarchical reference '" +
                                 std::string(e.as<sast::HierarchicalValueExpression>().symbol.name) + "'");
        case EK::UnaryOp: return lowerUnary(e);
        case EK::BinaryOp: return lowerBinary(e);
        case EK::ConditionalOp: {
            const auto& c = e.as<sast::ConditionalExpression>();
            if (c.conditions.size() != 1 || c.conditions[0].pattern)
                return opaque(e, "conditional expression with patterns");
            SigSpec cond = toBool(lower(*c.conditions[0].expr), e.sourceRange.start());
            SigSpec t = lower(c.left());
            SigSpec f = lower(c.right());
            return cell(CellKind::Mux, {f, t, cond}, w, e.sourceRange.start());
        }
        case EK::Concatenation: {
            SigSpec out;
            auto ops = e.as<sast::ConcatenationExpression>().operands();
            for (auto it = ops.rbegin(); it != ops.rend(); ++it) // last operand = least significant
                out.append(lower(**it));
            return out;
        }
        case EK::Replication: {
            const auto& r = e.as<sast::ReplicationExpression>();
            auto count = lower(r.count()).asConst();
            auto n = count ? count->toUint() : std::nullopt;
            if (!n)
                return opaque(e, "replication with non-constant count");
            return lower(r.concat()).repeated(static_cast<std::uint32_t>(*n));
        }
        case EK::ElementSelect:
        case EK::RangeSelect:
        case EK::MemberAccess: return lowerSelect(e);
        case EK::Conversion: {
            const auto& c = e.as<sast::ConversionExpression>();
            const auto& op = c.operand();
            if (!op.type->isIntegral() || !e.type->isIntegral()) {
                if (typeWidth(*op.type) == w && w > 0)
                    return lower(op); // e.g. unpacked array to same-size array
                return opaque(e, "conversion from '" + op.type->toString() + "' to '" + e.type->toString() + "'");
            }
            return lower(op).resized(w, op.type->isSigned());
        }
        case EK::Call: return lowerCall(e);
        case EK::EmptyArgument: return {};
        default:
            return opaque(e, "expression kind '" + std::string(sast::toString(e.kind)) + "'");
    }
}

SigSpec BodyBuilder::lowerUnary(const sast::Expression& e) {
    using Op = sast::UnaryOperator;
    const auto& u = e.as<sast::UnaryExpression>();
    const std::uint32_t w = exprWidth(e);
    const auto l = e.sourceRange.start();
    switch (u.op) {
        case Op::Plus: return lower(u.operand());
        case Op::Minus: return cell(CellKind::Neg, {lower(u.operand())}, w, l, e.type->isSigned());
        case Op::BitwiseNot: return cell(CellKind::Not, {lower(u.operand())}, w, l);
        case Op::BitwiseAnd: return cell(CellKind::ReduceAnd, {lower(u.operand())}, 1, l).resized(w, false);
        case Op::BitwiseOr: return cell(CellKind::ReduceOr, {lower(u.operand())}, 1, l).resized(w, false);
        case Op::BitwiseXor: return cell(CellKind::ReduceXor, {lower(u.operand())}, 1, l).resized(w, false);
        case Op::BitwiseNand:
            return cell(CellKind::Not, {cell(CellKind::ReduceAnd, {lower(u.operand())}, 1, l)}, 1, l).resized(w, false);
        case Op::BitwiseNor:
            return cell(CellKind::Not, {cell(CellKind::ReduceOr, {lower(u.operand())}, 1, l)}, 1, l).resized(w, false);
        case Op::BitwiseXnor:
            return cell(CellKind::Not, {cell(CellKind::ReduceXor, {lower(u.operand())}, 1, l)}, 1, l).resized(w, false);
        case Op::LogicalNot: return cell(CellKind::LogicNot, {lower(u.operand())}, 1, l).resized(w, false);
        default: return opaque(e, "increment/decrement inside an expression");
    }
}

namespace {

std::optional<CellKind> binaryKind(sast::BinaryOperator op) {
    using Op = sast::BinaryOperator;
    switch (op) {
        case Op::Add: return CellKind::Add;
        case Op::Subtract: return CellKind::Sub;
        case Op::Multiply: return CellKind::Mul;
        case Op::Divide: return CellKind::Div;
        case Op::Mod: return CellKind::Mod;
        case Op::BinaryAnd: return CellKind::And;
        case Op::BinaryOr: return CellKind::Or;
        case Op::BinaryXor: return CellKind::Xor;
        case Op::BinaryXnor: return CellKind::Xnor;
        case Op::Equality:
        case Op::CaseEquality: return CellKind::Eq;
        case Op::Inequality:
        case Op::CaseInequality: return CellKind::Ne;
        case Op::WildcardEquality: return CellKind::EqWild;
        case Op::GreaterThanEqual: return CellKind::Ge;
        case Op::GreaterThan: return CellKind::Gt;
        case Op::LessThanEqual: return CellKind::Le;
        case Op::LessThan: return CellKind::Lt;
        case Op::LogicalAnd: return CellKind::LogicAnd;
        case Op::LogicalOr: return CellKind::LogicOr;
        case Op::LogicalShiftLeft:
        case Op::ArithmeticShiftLeft: return CellKind::Shl;
        case Op::LogicalShiftRight: return CellKind::Shr;
        case Op::ArithmeticShiftRight: return CellKind::Sshr;
        case Op::Power: return CellKind::Pow;
        default: return std::nullopt;
    }
}

bool isComparison(CellKind k) {
    switch (k) {
        case CellKind::Eq:
        case CellKind::Ne:
        case CellKind::EqWild:
        case CellKind::Lt:
        case CellKind::Le:
        case CellKind::Gt:
        case CellKind::Ge:
        case CellKind::LogicAnd:
        case CellKind::LogicOr: return true;
        default: return false;
    }
}

} // namespace

SigSpec BodyBuilder::lowerBinary(const sast::Expression& e) {
    using Op = sast::BinaryOperator;
    const auto& b = e.as<sast::BinaryExpression>();
    const std::uint32_t w = exprWidth(e);
    const auto l = e.sourceRange.start();
    SigSpec lhs = lower(b.left());
    SigSpec rhs = lower(b.right());
    const bool isSigned = b.left().type->isSigned() && b.right().type->isSigned();

    switch (b.op) {
        case Op::WildcardInequality:
            return cell(CellKind::Not, {cell(CellKind::EqWild, {lhs, rhs}, 1, l)}, 1, l).resized(w, false);
        case Op::LogicalImplication:
            return cell(CellKind::LogicOr, {cell(CellKind::LogicNot, {lhs}, 1, l), rhs}, 1, l).resized(w, false);
        case Op::LogicalEquivalence:
            return cell(CellKind::Eq, {toBool(lhs, l), toBool(rhs, l)}, 1, l).resized(w, false);
        default: break;
    }
    auto kind = binaryKind(b.op);
    if (!kind)
        return opaque(e, "binary operator '" + std::string(sast::toString(b.op)) + "'");
    if (isComparison(*kind))
        return cell(*kind, {lhs, rhs}, 1, l, isSigned).resized(w, false);
    if (*kind == CellKind::Sshr || *kind == CellKind::Shl || *kind == CellKind::Shr)
        return cell(*kind, {lhs, rhs}, w, l, b.left().type->isSigned());
    return cell(*kind, {lhs, rhs}, w, l, isSigned);
}

SigSpec BodyBuilder::lowerSelect(const sast::Expression& e) {
    const std::uint32_t w = exprWidth(e);
    const auto l = e.sourceRange.start();

    if (e.kind == sast::ExpressionKind::MemberAccess) {
        const auto& ma = e.as<sast::MemberAccessExpression>();
        if (ma.member.kind != sast::SymbolKind::Field || !ma.value().type->isIntegral())
            return opaque(e, "member access on a non-packed type");
        const auto off = ma.member.as<sast::FieldSymbol>().bitOffset;
        SigSpec base = lower(ma.value());
        if (off + w > base.width())
            return opaque(e, "member access out of range");
        return base.extract(static_cast<std::uint32_t>(off), w);
    }

    const sast::Expression& valueExpr = e.kind == sast::ExpressionKind::ElementSelect
                                            ? e.as<sast::ElementSelectExpression>().value()
                                            : e.as<sast::RangeSelectExpression>().value();
    auto info = selectInfo(*valueExpr.type);
    if (!info)
        return opaque(e, "select from type '" + valueExpr.type->toString() + "'");
    SigSpec base = lower(valueExpr);

    auto staticSlice = [&](std::int64_t lowIndex, std::int64_t highIndex) -> SigSpec {
        // bits of elements lowIndex..highIndex (declared indices, any order)
        auto o1 = elementOffset(lowIndex, info->left, info->right);
        auto o2 = elementOffset(highIndex, info->left, info->right);
        if (!o1 || !o2)
            return SigSpec::allX(w);
        const std::uint32_t off = std::min(*o1, *o2) * info->elemWidth;
        if (off + w > base.width())
            return SigSpec::allX(w);
        return base.extract(off, w);
    };
    auto dynamicSelect = [&](SigSpec index, bool idxSigned) -> SigSpec {
        Cell c;
        c.kind = CellKind::DynSelect;
        c.inputs = {base, std::move(index)};
        c.width = w;
        c.isSigned = idxSigned;
        c.elemWidth = info->elemWidth;
        c.rangeLeft = info->left;
        c.rangeRight = info->right;
        c.origin = loc(l);
        return emit(std::move(c));
    };

    if (e.kind == sast::ExpressionKind::ElementSelect) {
        const auto& sel = e.as<sast::ElementSelectExpression>().selector();
        SigSpec idx = lower(sel);
        if (auto ci = constIndex(idx, sel.type->isSigned()))
            return staticSlice(*ci, *ci);
        return dynamicSelect(std::move(idx), sel.type->isSigned());
    }

    const auto& rs = e.as<sast::RangeSelectExpression>();
    SigSpec leftSig = lower(rs.left());
    auto leftConst = constIndex(leftSig, rs.left().type->isSigned());
    if (rs.getSelectionKind() == sast::RangeSelectionKind::Simple) {
        auto rightConst = constIndex(lower(rs.right()), rs.right().type->isSigned());
        if (!leftConst || !rightConst)
            return opaque(e, "range select with non-constant bounds");
        return staticSlice(*leftConst, *rightConst);
    }
    // Indexed part-select: base +: count / base -: count
    auto countConst = constIndex(lower(rs.right()), false);
    if (!countConst || *countConst <= 0)
        return opaque(e, "indexed part-select with non-constant width");
    const std::int64_t count = *countConst;
    const bool up = rs.getSelectionKind() == sast::RangeSelectionKind::IndexedUp;
    if (leftConst) {
        const std::int64_t other = up ? *leftConst + count - 1 : *leftConst - count + 1;
        return staticSlice(*leftConst, other);
    }
    // Dynamic base: compute the declared index of the least significant element.
    const bool descending = info->left >= info->right;
    const bool needsAdjust = (up && !descending) || (!up && descending);
    SigSpec lsbIndex = leftSig;
    if (needsAdjust) {
        const auto iw = leftSig.width();
        lsbIndex = cell(up ? CellKind::Add : CellKind::Sub,
                        {leftSig, SigSpec::constant(static_cast<std::uint64_t>(count - 1), iw)}, iw, l);
    }
    return dynamicSelect(std::move(lsbIndex), rs.left().type->isSigned());
}

SigSpec BodyBuilder::lowerCall(const sast::Expression& e) {
    const auto& call = e.as<sast::CallExpression>();
    const std::uint32_t w = exprWidth(e);
    if (call.isSystemCall()) {
        const auto name = call.getSubroutineName();
        if ((name == "$signed" || name == "$unsigned") && call.arguments().size() == 1)
            return lower(*call.arguments()[0]).resized(w, call.arguments()[0]->type->isSigned());
        sast::EvalContext ctx(sym_);
        auto cv = e.eval(ctx);
        if (cv.isInteger())
            return SigSpec::constant(fromSVInt(cv.integer(), w));
        return opaque(e, "system function '" + std::string(name) + "'");
    }
    // Constant user-function calls (e.g. clog2 helpers) fold to constants.
    {
        sast::EvalContext ctx(sym_);
        auto cv = e.eval(ctx);
        if (cv.isInteger())
            return SigSpec::constant(fromSVInt(cv.integer(), w));
    }
    return inlineFunction(e);
}

// ============================================================================
// LValues
// ============================================================================

std::optional<std::vector<LTarget>> BodyBuilder::lvalue(const sast::Expression& e) {
    std::vector<LTarget> out;
    if (!lvalueInto(e, out)) {
        unsupported("IR-UNSUPPORTED-LVALUE", "assignment target '" + std::string(sast::toString(e.kind)) + "'",
                    e.sourceRange.start());
        return std::nullopt;
    }
    return out;
}

bool BodyBuilder::lvalueInto(const sast::Expression& e, std::vector<LTarget>& out) {
    using EK = sast::ExpressionKind;
    switch (e.kind) {
        case EK::NamedValue: {
            const auto& s = e.as<sast::NamedValueExpression>().symbol;
            if (s.kind != sast::SymbolKind::Net && s.kind != sast::SymbolKind::Variable &&
                s.kind != sast::SymbolKind::FormalArgument)
                return false;
            const auto w = widthOf(s);
            if (w == 0)
                return false;
            out.push_back(wholeTarget(&s, w));
            return true;
        }
        case EK::Concatenation: {
            auto ops = e.as<sast::ConcatenationExpression>().operands();
            for (auto it = ops.rbegin(); it != ops.rend(); ++it)
                if (!lvalueInto(**it, out))
                    return false;
            return true;
        }
        case EK::Conversion: return lvalueInto(e.as<sast::ConversionExpression>().operand(), out);
        case EK::MemberAccess: {
            const auto& ma = e.as<sast::MemberAccessExpression>();
            if (ma.member.kind != sast::SymbolKind::Field || !ma.value().type->isIntegral())
                return false;
            std::vector<LTarget> inner;
            if (!lvalueInto(ma.value(), inner) || inner.size() != 1 || inner[0].dynamic)
                return false;
            LTarget t = inner[0];
            t.offset += static_cast<std::uint32_t>(ma.member.as<sast::FieldSymbol>().bitOffset);
            t.width = exprWidth(e);
            out.push_back(t);
            return true;
        }
        case EK::ElementSelect:
        case EK::RangeSelect: {
            const auto& valueExpr = e.kind == EK::ElementSelect ? e.as<sast::ElementSelectExpression>().value()
                                                                : e.as<sast::RangeSelectExpression>().value();
            std::vector<LTarget> inner;
            if (!lvalueInto(valueExpr, inner) || inner.size() != 1 || inner[0].dynamic)
                return false;
            auto info = selectInfo(*valueExpr.type);
            if (!info)
                return false;
            LTarget t = inner[0];
            const std::uint32_t w = exprWidth(e);

            auto setStatic = [&](std::int64_t i1, std::int64_t i2) -> bool {
                auto o1 = elementOffset(i1, info->left, info->right);
                auto o2 = elementOffset(i2, info->left, info->right);
                if (!o1 || !o2)
                    return false;
                t.offset += std::min(*o1, *o2) * info->elemWidth;
                t.width = w;
                return true;
            };

            if (e.kind == EK::ElementSelect) {
                const auto& sel = e.as<sast::ElementSelectExpression>().selector();
                SigSpec idx = lower(sel);
                if (auto ci = constIndex(idx, sel.type->isSigned())) {
                    if (!setStatic(*ci, *ci)) {
                        warn("IR-OUT-OF-RANGE", Severity::Warning, "constant index out of range; assignment ignored",
                             e.sourceRange.start());
                        t.width = 0;
                    }
                }
                else {
                    t.dynamic = true;
                    t.index = std::move(idx);
                    t.indexSigned = sel.type->isSigned();
                    t.regionWidth = t.width;
                    t.width = w;
                    t.unitWidth = info->elemWidth;
                    t.left = info->left;
                    t.right = info->right;
                }
                out.push_back(t);
                return true;
            }
            const auto& rs = e.as<sast::RangeSelectExpression>();
            SigSpec leftSig = lower(rs.left());
            auto leftConst = constIndex(leftSig, rs.left().type->isSigned());
            auto rightConst = constIndex(lower(rs.right()), rs.right().type->isSigned());
            if (!leftConst && rightConst && *rightConst > 0 &&
                rs.getSelectionKind() != sast::RangeSelectionKind::Simple) {
                // x[base +: N] <= ... with a variable base: dynamic insert of
                // N elements starting at the least significant element.
                const bool up = rs.getSelectionKind() == sast::RangeSelectionKind::IndexedUp;
                const bool descending = info->left >= info->right;
                SigSpec lsbIndex = leftSig;
                if ((up && !descending) || (!up && descending)) {
                    const auto iw = leftSig.width();
                    lsbIndex = cell(up ? CellKind::Add : CellKind::Sub,
                                    {leftSig, SigSpec::constant(static_cast<std::uint64_t>(*rightConst - 1), iw)}, iw,
                                    e.sourceRange.start());
                }
                t.dynamic = true;
                t.index = std::move(lsbIndex);
                t.indexSigned = rs.left().type->isSigned();
                t.regionWidth = t.width;
                t.width = w;
                t.unitWidth = info->elemWidth;
                t.left = info->left;
                t.right = info->right;
                out.push_back(t);
                return true;
            }
            if (!leftConst || !rightConst)
                return false; // non-constant simple range as assignment target
            std::int64_t other = *rightConst;
            if (rs.getSelectionKind() == sast::RangeSelectionKind::IndexedUp)
                other = *leftConst + *rightConst - 1;
            else if (rs.getSelectionKind() == sast::RangeSelectionKind::IndexedDown)
                other = *leftConst - *rightConst + 1;
            if (!setStatic(*leftConst, other))
                return false;
            out.push_back(t);
            return true;
        }
        default: return false;
    }
}

} // namespace opencdc::frontend::detail
