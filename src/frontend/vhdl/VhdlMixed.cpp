// Mixed-language designs: VHDL entities instantiated from Verilog and
// Verilog modules instantiated from VHDL. Values cross the language border
// as ir::InstanceParam (integral bits, strings, reals); ports are bound by
// name (any case) or position, with the Verilog rule for width mismatches
// (zero extension / truncation).

#include "frontend/vhdl/VhdlElab.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace opencdc::vhdl {

namespace {

std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// Verilog strings are integers: 8 bits per character, first character in
/// the most significant byte; leading zero bytes are padding.
std::optional<std::string> bitsToString(const ir::ConstBits& b) {
    if (!b.isFullyDefined() || b.width() % 8 != 0)
        return std::nullopt;
    std::string out;
    for (std::uint32_t byte = b.width() / 8; byte-- > 0;) {
        unsigned c = 0;
        for (std::uint32_t k = 0; k < 8; ++k)
            if (b[byte * 8 + k] == ir::Logic::L1)
                c |= 1U << k;
        if (c == 0 && out.empty())
            continue;
        if (c < 0x20 || c >= 0x7f)
            return std::nullopt;
        out += static_cast<char>(c);
    }
    return out;
}

std::optional<std::int64_t> bitsToInt(const ir::InstanceParam& p) {
    if (!p.bits)
        return std::nullopt;
    if (p.isSigned)
        return p.bits->toInt();
    auto u = p.bits->toUint();
    if (!u || *u > static_cast<std::uint64_t>(INT64_MAX))
        return std::nullopt;
    return static_cast<std::int64_t>(*u);
}

} // namespace

bool Elaborator::hasEntity(const std::string& name) {
    return findEntity("work", lower(name), nullptr) != nullptr;
}

// ---------------------------------------------------------------------------
// Values across the border
// ---------------------------------------------------------------------------

SVal Elaborator::fromForeign(const ir::InstanceParam& p, const Type* t, Loc l) {
    if (!t) {
        error("VHDL-GENERIC", "generic without a type", l);
        return {};
    }
    const Type& B = t->baseType();
    auto bad = [&](const std::string& what) {
        error("VHDL-GENERIC", "parameter value for '" + p.name + "' cannot be converted to '" + t->name + "' (" +
                                  what + ")",
              l);
        return SVal{};
    };
    if (B.isIntegerLike() || B.kind == TypeKind::Physical) {
        if (p.str)
            return bad("a string");
        auto v = bitsToInt(p);
        if (!v && p.real)
            v = static_cast<std::int64_t>(std::llround(*p.real));
        if (!v)
            return bad("not an integer");
        return makeConst(t, Value::integer(*v));
    }
    if (B.isRealLike()) {
        if (p.str)
            return bad("a string");
        if (p.real)
            return makeConst(t, Value::real(*p.real));
        if (auto v = bitsToInt(p))
            return makeConst(t, Value::real(static_cast<double>(*v)));
        return bad("not a number");
    }
    if (B.tag == TypeTag::Boolean) {
        if (p.str) {
            const std::string s = lower(*p.str);
            if (s == "true")
                return makeConst(t, Value::integer(1));
            if (s == "false")
                return makeConst(t, Value::integer(0));
        }
        if (p.bits && p.bits->isFullyDefined())
            return makeConst(t, Value::integer(p.bits->isZero() ? 0 : 1));
        return bad("not a boolean");
    }
    if (B.tag == TypeTag::StdUlogic || B.tag == TypeTag::Bit) {
        if (!p.bits || p.bits->width() == 0)
            return bad("not a bit");
        return makeBits(t, ir::SigSpec::constant(p.bits->extract(0, 1)));
    }
    if (B.isStringLike() && B.element && B.element->baseTag() == TypeTag::Character) {
        std::optional<std::string> s = p.str;
        if (!s && p.bits)
            s = bitsToString(*p.bits);
        if (!s)
            return bad("not a string");
        return evalStringLiteral(*s, t, l);
    }
    if (B.isLogicVector()) {
        if (!p.bits)
            return bad("not a vector");
        std::uint32_t w = p.bits->width();
        const Type* ct = t;
        if (t->isFullyConstrained()) {
            w = bitWidth(*t).value_or(w);
        }
        else {
            const Type* it = t->indexTypes.empty() ? std_.natural : t->indexTypes.front();
            (void)it;
            ct = types.constrainedArray(t, {IntRange{static_cast<std::int64_t>(w) - 1, 0, false}});
        }
        ir::SigSpec bits = ir::SigSpec::constant(*p.bits).resized(w, p.isSigned);
        return makeBits(ct, bits);
    }
    return bad("unsupported generic type");
}

std::optional<ir::InstanceParam> Elaborator::toForeign(const SVal& v) {
    if (!v.known() || !v.type)
        return std::nullopt;
    const Type& B = v.type->baseType();
    ir::InstanceParam p;
    if (B.isIntegerLike() || B.kind == TypeKind::Physical) {
        p.bits = ir::ConstBits::fromUint(static_cast<std::uint64_t>(v.value->i), 32);
        p.isSigned = true;
        return p;
    }
    if (B.isRealLike()) {
        p.real = v.value->r;
        return p;
    }
    if (B.tag == TypeTag::Boolean) {
        p.bits = ir::ConstBits::fromUint(v.value->i ? 1 : 0, 32);
        p.isSigned = true;
        return p;
    }
    if (B.isStringLike() && B.element && B.element->baseTag() == TypeTag::Character) {
        std::string s;
        for (const auto& e : v.value->elems) {
            const auto& keys = B.element->baseType().enumInfo->keys;
            const auto i = static_cast<std::size_t>(e.i);
            s += i < keys.size() && keys[i].size() == 3 ? keys[i][1] : '?';
        }
        p.str = s;
        return p;
    }
    if (auto bits = encode(*v.type, *v.value)) {
        p.bits = *bits;
        p.isSigned = B.tag == TypeTag::Signed || B.tag == TypeTag::ArithSigned;
        return p;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// VHDL entity instantiated from another language
// ---------------------------------------------------------------------------

ir::ModuleId Elaborator::foreignEntity(const std::string& name, const std::vector<ir::InstanceParam>& params,
                                       const SourceLocation& where) {
    if (!design_)
        design_ = opts_.design ? opts_.design : std::make_shared<ir::Design>();
    std::string libName;
    const DesignUnit* e = findEntity("work", lower(name), &libName);
    if (!e)
        return {};
    const DesignUnit* a = defaultArchitecture(libName, *e);
    if (!a) {
        warn("VHDL-ARCHITECTURE", "entity '" + e->name.text + "' has no architecture", e->loc);
        return {};
    }
    std::string canon;
    std::string text;
    foreignParams_ = &params;
    const int before = errors_;
    Scope* gs = bindGenerics(*e, libName, nullptr, nullptr, nullptr, e->loc, canon, text);
    foreignParams_ = nullptr;
    if (!gs) {
        if (errors_ > before)
            sink_.report({"VHDL-MIXED", Severity::Error,
                          "cannot bind the parameters of the instance of VHDL entity '" + e->name.text + "'", where, {}});
        return {};
    }
    auto specs = portSpecs(*e, gs);
    for (const auto& ps : specs)
        if (ps.type && !ps.type->isFullyConstrained()) {
            error("VHDL-UNCONSTRAINED", "port '" + ps.display + "' of '" + e->name.text +
                                            "' is unconstrained; it cannot be instantiated from Verilog",
                  ps.loc);
            return {};
        }
    return bodyFor(*e, *a, libName, gs, specs, canon, text, e->loc);
}

std::vector<Elaborator::ForeignPort> Elaborator::foreignPorts(ir::ModuleId id) const {
    std::vector<ForeignPort> out;
    auto si = portSpecs_.find(id.index());
    auto pi = portPieces_.find(id.index());
    if (si == portSpecs_.end() || pi == portPieces_.end())
        return out;
    for (const auto& ps : si->second) {
        ForeignPort fp;
        fp.name = ps.display;
        fp.dir = ps.mode == Mode::In ? ir::PortDir::In : ps.mode == Mode::InOut ? ir::PortDir::InOut : ir::PortDir::Out;
        fp.width = ps.type ? bitWidth(*ps.type).value_or(0) : 0;
        out.push_back(std::move(fp));
    }
    for (std::size_t k = 0; k < pi->second.size(); ++k) {
        const PortPiece& p = pi->second[k];
        if (p.port < out.size())
            out[p.port].pieces.emplace_back(k, p.offset);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Verilog module instantiated from VHDL
// ---------------------------------------------------------------------------

void Elaborator::elaborateForeignInstance(const Stmt& s, Scope& scope, const std::string& module) {
    const std::string instName = body->prefix + s.label.text;
    Ctx pctx;
    pctx.scope = &scope;
    auto evalActual = [&](const Expr& e, const Type* expected) -> SVal {
        if (!expected && (e.kind == ExprKind::String))
            expected = std_.string;
        if (!expected && e.kind == ExprKind::BitString)
            expected = std_.stdLogicVector;
        return eval(e, pctx, expected);
    };

    // parameters
    std::vector<ir::InstanceParam> params;
    for (const auto& a : s.genericMap) {
        if (!a.actual || a.actual->kind == ExprKind::Open)
            continue;
        SVal v = evalActual(*a.actual, nullptr);
        if (!v.valid())
            return;
        if (!v.known()) {
            error("VHDL-GENERIC", "the value of a parameter of Verilog module '" + module + "' is not static",
                  a.actual->loc);
            return;
        }
        auto p = toForeign(v);
        if (!p) {
            error("VHDL-GENERIC", "value of type '" + v.type->name + "' cannot be passed to Verilog module '" +
                                      module + "'",
                  a.actual->loc);
            return;
        }
        if (a.formal) {
            const Expr* f = a.formal.get();
            p->name = f->ident.text;
        }
        params.push_back(std::move(*p));
    }
    const ir::ModuleId cid = opts_.resolveForeign(module, params);
    ir::ChildInstance ci;
    ci.name = instName;
    ci.definition = module;
    ci.loc = srcLoc(s.loc);
    if (!cid.valid()) {
        // requested; available in the next elaboration round
        ci.unresolvedParams = std::move(params);
        body->body.children.push_back(std::move(ci));
        return;
    }
    ci.body = cid;
    const auto& ports = design_->body(cid).ports;
    std::vector<ir::SigSpec> actual(ports.size());
    std::vector<bool> given(ports.size(), false);
    std::size_t pos = 0;
    for (const auto& a : s.portMap) {
        std::optional<std::size_t> k;
        if (!a.formal) {
            if (pos < ports.size())
                k = pos;
            ++pos;
        }
        else if (a.formal->kind == ExprKind::Simple) {
            for (std::size_t i = 0; i < ports.size(); ++i)
                if (lower(ports[i].name) == a.formal->ident.key)
                    k = i;
        }
        else {
            unsupported("IR-UNSUPPORTED-PORT", "partial association of a port of Verilog module '" + module + "'",
                        a.loc);
            continue;
        }
        if (!k) {
            error("VHDL-PORT-MAP", "Verilog module '" + module + "' has no port '" +
                                       (a.formal ? a.formal->ident.text : std::to_string(pos)) + "'",
                  a.loc);
            continue;
        }
        if (!a.actual || a.actual->kind == ExprKind::Open)
            continue;
        const Expr* act = a.actual->kind == ExprKind::Inertial ? a.actual->lhs.get() : a.actual.get();
        const auto& port = ports[*k];
        const std::uint32_t w = design_->body(cid).net(port.net).width;
        ir::SigSpec bits;
        if (port.dir == ir::PortDir::In) {
            const Type* expected = w == 1 ? std_.stdLogic : types.constrainedArray(std_.stdLogicVector,
                                                                                   {IntRange{w - 1, 0, false}});
            TypeSet ts = typesOf(*act, pctx);
            if (ts.types.size() == 1 && bitWidth(*ts.types.front()))
                expected = ts.types.front();
            SVal v = evalActual(*act, expected);
            if (!v.valid())
                continue;
            const bool sgn = v.type->baseTag() == TypeTag::Signed || isSignedInteger(*v.type);
            bits = bitsOf(v, a.loc).resized(w, sgn);
        }
        else {
            std::vector<Target> tg;
            if (!targets(*act, pctx, tg))
                continue;
            bool ok = true;
            for (auto it = tg.rbegin(); it != tg.rend(); ++it) {
                const Object& o = object(it->object);
                if (it->dynamic || !o.isSignal || o.storage.empty()) {
                    ok = false;
                    break;
                }
                bits.append(o.storage.extract(it->offset, it->width));
            }
            if (!ok) {
                unsupported("IR-UNSUPPORTED-PORT", "output port '" + port.name + "' of '" + instName +
                                                       "' connected to an unsupported expression",
                            a.loc);
                continue;
            }
            if (bits.width() != w)
                warn("VHDL-MIXED-WIDTH", "port '" + port.name + "' of Verilog module '" + module + "' has " +
                                             std::to_string(w) + " bit(s), the actual has " +
                                             std::to_string(bits.width()),
                     a.loc);
            if (bits.width() > w)
                bits = bits.extract(0, w);
        }
        actual[*k] = bits;
        given[*k] = true;
    }
    for (std::size_t k = 0; k < ports.size(); ++k) {
        ir::PortBinding pb;
        pb.port = ports[k].name;
        pb.dir = ports[k].dir;
        pb.childNet = ports[k].net;
        if (given[k])
            pb.actual = actual[k];
        ci.ports.push_back(std::move(pb));
    }
    body->body.children.push_back(std::move(ci));
}

} // namespace opencdc::vhdl
