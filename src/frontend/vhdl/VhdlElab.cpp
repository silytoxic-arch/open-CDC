// VHDL elaboration: design hierarchy (entities, architectures, generics,
// port maps, generate statements, blocks) and storage (signals, ports,
// process variables) of each unique body.

#include "frontend/vhdl/VhdlElab.h"

#include "frontend/vhdl/VhdlParser.h"

#include <algorithm>
#include <set>

namespace opencdc::vhdl {

using ir::SigSpec;

// ---------------------------------------------------------------------------
// Construction, diagnostics
// ---------------------------------------------------------------------------

Elaborator::Elaborator(std::vector<SourceFileEntry>& files, DiagnosticSink& sink, ElabOptions opts)
    : files_(files), sink_(sink), opts_(std::move(opts)) {
    root = newScope(nullptr, "root");
    createBuiltins();
    indexLibraries();
}

Elaborator::~Elaborator() = default;

SourceLocation Elaborator::srcLoc(Loc l) const {
    SourceLocation out;
    if (!l.valid() || l.file >= files_.size())
        return out;
    out.file = files_[l.file].path;
    out.line = l.line;
    out.column = l.col;
    return out;
}

void Elaborator::error(const std::string& code, const std::string& msg, Loc l) {
    const std::string key = "E" + code + msg + std::to_string(l.file) + ":" + std::to_string(l.line) + ":" +
                            std::to_string(l.col);
    if (!reported_.insert(key).second)
        return;
    ++errors_;
    sink_.report({code, Severity::Error, msg, srcLoc(l), {}});
}

void Elaborator::warn(const std::string& code, const std::string& msg, Loc l, Severity sev) {
    const std::string key = "W" + code + msg + std::to_string(l.file) + ":" + std::to_string(l.line) + ":" +
                            std::to_string(l.col);
    if (!reported_.insert(key).second)
        return;
    sink_.report({code, sev, msg, srcLoc(l), {}});
}

void Elaborator::unsupported(const std::string& code, const std::string& what, Loc l) {
    const std::string key = "U" + code + what + std::to_string(l.file) + ":" + std::to_string(l.line) + ":" +
                            std::to_string(l.col);
    if (!reported_.insert(key).second)
        return;
    if (design_)
        design_->unsupported.push_back({code, body ? body->body.name : std::string(), what, srcLoc(l)});
    sink_.report({code, Severity::Warning, what + " (not analyzed; modelled conservatively where possible)",
                  srcLoc(l), {}});
}

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------

namespace {

ir::PortDir dirOf(Mode m) {
    switch (m) {
        case Mode::In: return ir::PortDir::In;
        case Mode::Out:
        case Mode::Buffer: return ir::PortDir::Out;
        case Mode::InOut: return ir::PortDir::InOut;
        default: return ir::PortDir::None;
    }
}

std::int32_t clamp32(std::int64_t v) {
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(v, INT32_MIN, INT32_MAX));
}

} // namespace

int Elaborator::createStorageObject(const std::string& name, const Type* t, bool signal, Mode mode, Loc l,
                                    const std::vector<ir::Attribute>& attrs, const SVal* init) {
    Object o;
    o.name = name;
    o.type = t;
    o.isSignal = signal;
    o.isPort = mode != Mode::None;
    o.mode = mode;
    o.loc = l;
    std::optional<Value> initValue;
    if (init && init->valid()) {
        Ctx c;
        c.scope = root;
        c.constOnly = true;
        SVal v = convert(*init, t, c, l);
        if (v.known()) {
            initValue = *v.value;
            o.initial = v;
        }
        else if (v.valid()) {
            unsupported("IR-UNSUPPORTED-INIT", "non-constant initial value of '" + name + "'", l);
        }
    }
    if (!o.initial.valid())
        o.initial = initialValue(o, l);
    const int idx = static_cast<int>(body->objects.size());
    const auto W = bitWidth(*t);
    if (!W) {
        if (signal)
            unsupported("IR-UNSUPPORTED-TYPE", "signal '" + name + "' of type '" + t->name + "'", l);
        body->objects.push_back(std::move(o));
        return idx;
    }
    if (*W == 0) {
        body->objects.push_back(std::move(o));
        return idx;
    }
    if (*W > kMaxObjectBits) {
        error("VHDL-TOO-WIDE", "'" + name + "' has " + std::to_string(*W) + " bits (more than " +
                                   std::to_string(kMaxObjectBits) + ")",
              l);
        body->objects.push_back(std::move(o));
        return idx;
    }
    std::function<SigSpec(const std::string&, const Type&, const std::optional<Value>&)> leaf =
        [&](const std::string& n, const Type& ty, const std::optional<Value>& iv) -> SigSpec {
        if (ty.kind == TypeKind::Record && !ty.fields.empty()) {
            SigSpec s;
            for (std::size_t k = ty.fields.size(); k-- > 0;) {
                std::optional<Value> fv;
                if (iv && iv->kind == Value::Kind::Composite && k < iv->elems.size())
                    fv = iv->elems[k];
                SigSpec f = leaf(n + "." + ty.fields[k].display, *ty.fields[k].type, fv);
                s.append(f);
            }
            return s;
        }
        const std::uint32_t w = bitWidth(ty).value_or(0);
        if (w == 0)
            return {};
        ir::Net net;
        net.name = n;
        net.width = w;
        net.kind = signal ? ir::NetKind::Wire : ir::NetKind::Variable;
        net.dir = dirOf(mode);
        net.loc = srcLoc(l);
        net.attrs = attrs;
        net.msb = clamp32(static_cast<std::int64_t>(w) - 1);
        net.lsb = 0;
        if (ty.kind == TypeKind::Array && ty.indexRanges.size() == 1 && ty.indexRanges[0]) {
            const IntRange& r = *ty.indexRanges[0];
            if (ty.element && ty.element->isScalar() && bitWidth(*ty.element).value_or(0) == 1) {
                net.msb = clamp32(r.left);
                net.lsb = clamp32(r.right);
                net.isSigned = ty.baseTag() == TypeTag::Signed || ty.baseTag() == TypeTag::ArithSigned;
            }
            else if (ty.element) {
                // array of vectors / records: a memory-like array (element granularity)
                net.arrayDepth = static_cast<std::uint32_t>(r.length());
                net.elemWidth = bitWidth(*ty.element).value_or(0);
                net.msb = clamp32(r.left);
                net.lsb = clamp32(r.right);
                if (net.arrayDepth == 0 || net.elemWidth == 0 || net.arrayDepth * net.elemWidth != w) {
                    net.arrayDepth = 0;
                    net.elemWidth = 0;
                    net.msb = clamp32(static_cast<std::int64_t>(w) - 1);
                    net.lsb = 0;
                }
            }
        }
        else if (ty.kind == TypeKind::Array && ty.indexRanges.size() > 1 && ty.element) {
            net.elemWidth = bitWidth(*ty.element).value_or(0);
            net.arrayDepth = net.elemWidth ? w / net.elemWidth : 0;
            if (net.arrayDepth * net.elemWidth != w) {
                net.arrayDepth = 0;
                net.elemWidth = 0;
            }
        }
        else if (ty.isIntegerLike()) {
            net.isSigned = isSignedInteger(ty);
        }
        if (iv) {
            if (auto bits = encode(ty, *iv))
                net.init = *bits;
        }
        const ir::NetId id = body->body.addNet(std::move(net));
        body->netOwner[static_cast<std::uint32_t>(id.index())] = static_cast<std::uint32_t>(idx);
        return SigSpec::net(id, w);
    };
    o.storage = leaf(name, *t, initValue);
    body->objects.push_back(std::move(o));
    return idx;
}

void Elaborator::elaborateDeclarativePart(Scope& scope, const std::vector<DeclPtr>& decls, Ctx& ctx) {
    for (const auto& d : decls)
        declare(scope, *d, ctx);
}

// ---------------------------------------------------------------------------
// Library units
// ---------------------------------------------------------------------------

const DesignUnit* Elaborator::findEntity(const std::string& lib, const std::string& name, std::string* foundLib) {
    if (LibraryUnits* l = library(lib)) {
        if (auto it = l->entities.find(name); it != l->entities.end()) {
            if (foundLib)
                *foundLib = lib;
            return it->second;
        }
    }
    // default binding of components: any library
    for (auto& [ln, l] : libraries) {
        if (auto it = l.entities.find(name); it != l.entities.end()) {
            if (foundLib)
                *foundLib = ln;
            return it->second;
        }
    }
    return nullptr;
}

const DesignUnit* Elaborator::defaultArchitecture(const std::string& lib, const DesignUnit& entity) {
    LibraryUnits* l = library(lib);
    if (!l)
        return nullptr;
    auto it = l->architectures.find(entity.name.key);
    if (it == l->architectures.end() || it->second.empty())
        return nullptr;
    return it->second.back(); // most recently analyzed
}

// ---------------------------------------------------------------------------
// Generics and ports
// ---------------------------------------------------------------------------

namespace {

/// Base port/generic name of a formal designator ("p", "p(3)", "p.f").
const Expr* formalBase(const Expr* e) {
    while (e && (e->kind == ExprKind::Call || e->kind == ExprKind::Selected))
        e = e->prefix.get();
    return e && e->kind == ExprKind::Simple ? e : nullptr;
}

} // namespace

Scope* Elaborator::bindGenerics(const DesignUnit& entity, const std::string& lib, const std::vector<Assoc>* map,
                                const Decl* component, Ctx* parent, Loc l, std::string& canon, std::string& params) {
    const int errorsBefore = errors_;
    Scope* gs = newScope(root, lib + "." + entity.name.text);
    gs->workLibrary = lib;
    applyContext(*gs, entity.context, lib);
    Ctx ctx;
    ctx.scope = gs;
    ctx.constOnly = true;

    // formal name -> actual
    std::map<std::string, const Expr*> actuals;
    std::vector<std::string> positional;
    const auto& order = component ? component->generics : entity.generics;
    for (const auto& g : order)
        for (const auto& n : g.names)
            positional.push_back(n.key);
    if (map) {
        std::size_t pos = 0;
        for (const auto& a : *map) {
            if (!a.formal) {
                if (pos < positional.size())
                    actuals[positional[pos]] = a.actual.get();
                else
                    error("VHDL-GENERIC-MAP", "too many generics in the generic map of '" + entity.name.text + "'",
                          a.loc);
                ++pos;
                continue;
            }
            const Expr* b = formalBase(a.formal.get());
            if (!b) {
                unsupported("IR-UNSUPPORTED-GENERIC", "generic association with this formal", a.loc);
                continue;
            }
            actuals[b->ident.key] = a.actual.get();
        }
    }
    std::vector<ExprPtr> overrideExprs;
    if (!parent) {
        for (const auto& [name, text] : opts_.generics) {
            std::vector<ParseMessage> msgs;
            ExprPtr e = parseVhdlExpression(text, msgs);
            if (!e) {
                error("VHDL-GENERIC", "cannot parse the value '" + text + "' of generic '" + name + "'", l);
                continue;
            }
            std::string key = name;
            std::ranges::transform(key, key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            actuals[key] = e.get();
            overrideExprs.push_back(std::move(e));
        }
    }
    // generic values from an instance in another language (mixed designs)
    std::map<std::string, const ir::InstanceParam*> foreign;
    if (foreignParams_) {
        std::vector<std::string> order2;
        for (const auto& g : entity.generics)
            if (g.cls != InterfaceDecl::Class::Type)
                for (const auto& n : g.names)
                    order2.push_back(n.key);
        std::size_t pos = 0;
        for (const auto& fp : *foreignParams_) {
            std::string key = fp.name;
            std::ranges::transform(key, key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (key.empty()) {
                if (pos >= order2.size()) {
                    error("VHDL-GENERIC-MAP", "too many parameters for entity '" + entity.name.text + "'", l);
                    continue;
                }
                key = order2[pos++];
            }
            foreign[key] = &fp;
        }
    }
    std::set<std::string> known;
    for (const auto& g : entity.generics) {
        if (g.cls == InterfaceDecl::Class::Type) {
            for (const auto& n : g.names) {
                Symbol* s = newSymbol(SymKind::Type, n.key, n.text, n.loc);
                s->type = std_.integer;
                if (auto it = actuals.find(n.key); it != actuals.end() && it->second && parent) {
                    if (const Type* t = typeMark(*it->second, *parent))
                        s->type = t;
                }
                canon += n.key + "=" + (s->type ? s->type->name : std::string("?")) + ";";
                gs->add(s);
                known.insert(n.key);
            }
            continue;
        }
        declareInterface(*gs, g, ctx);
        for (const auto& n : g.names) {
            known.insert(n.key);
            const auto* v = gs->local(n.key);
            if (!v || v->empty())
                continue;
            Symbol* s = v->back();
            // declared type: an unconstrained generic took the constraint of its default
            const Type* declared = g.subtype ? subtype(*g.subtype, ctx) : s->type;
            if (declared && declared->isArray() && !declared->isFullyConstrained())
                s->type = declared;
            const Expr* actual = nullptr;
            if (auto it = actuals.find(n.key); it != actuals.end())
                actual = it->second;
            if (actual && actual->kind != ExprKind::Open) {
                Ctx& ac = parent ? *parent : ctx;
                SVal val = eval(*actual, ac, s->type);
                if (val.valid() && s->type)
                    val = convert(val, s->type, ac, actual->loc);
                if (val.valid()) {
                    if (s->type && s->type->isArray() && !s->type->isFullyConstrained())
                        s->type = val.type;
                    s->value = val;
                }
            }
            else if (auto fit = foreign.find(n.key); !actual && fit != foreign.end()) {
                SVal val = fromForeign(*fit->second, s->type, l);
                if (val.valid()) {
                    if (s->type && s->type->isArray() && !s->type->isFullyConstrained())
                        s->type = val.type;
                    s->value = val;
                }
            }
            else if (!actual && component) {
                // default from the component declaration
                for (const auto& cg : component->generics)
                    for (const auto& cn : cg.names)
                        if (cn.key == n.key && cg.init && parent) {
                            SVal val = eval(*cg.init, *parent, s->type);
                            if (val.valid() && s->type)
                                val = convert(val, s->type, *parent, cg.loc);
                            if (val.valid())
                                s->value = val;
                        }
            }
            if (!s->value.valid()) {
                error("VHDL-GENERIC", "generic '" + n.text + "' of '" + entity.name.text + "' has no value", l);
                continue;
            }
            if (s->type && s->type->isArray() && !s->type->isFullyConstrained())
                s->type = s->value.type; // default value kept
            if (!s->value.known()) {
                error("VHDL-GENERIC", "the value of generic '" + n.text + "' of '" + entity.name.text +
                                          "' is not static",
                      actual && parent ? actual->loc : l);
                continue;
            }
            const std::string text = formatValue(*s->value.type, *s->value.value);
            canon += n.key + "=" + text + ";";
            if (!params.empty())
                params += ", ";
            params += n.text + "=" + text;
        }
    }
    for (const auto& [k, e] : actuals)
        if (!known.contains(k) && e)
            error("VHDL-GENERIC-MAP", "entity '" + entity.name.text + "' has no generic '" + k + "'",
                  parent ? e->loc : l); // top-level overrides have no source location
    for (const auto& [k, fp] : foreign)
        if (!known.contains(k))
            error("VHDL-GENERIC-MAP", "entity '" + entity.name.text + "' has no generic '" + fp->name + "'", l);
    if (!params.empty())
        params = "#(" + params + ")";
    // a body with missing or non-static generics would only produce follow-up errors
    return errors_ > errorsBefore ? nullptr : gs;
}

std::vector<Elaborator::PortSpec> Elaborator::portSpecs(const DesignUnit& entity, Scope* entityScope) {
    std::vector<PortSpec> out;
    Ctx ctx;
    ctx.scope = entityScope;
    ctx.constOnly = true;
    for (const auto& p : entity.ports) {
        const Type* t = p.subtype ? subtype(*p.subtype, ctx) : nullptr;
        for (const auto& n : p.names) {
            PortSpec ps;
            ps.key = n.key;
            ps.display = n.text;
            ps.mode = p.mode == Mode::None ? Mode::In : p.mode;
            ps.type = t;
            ps.defaultExpr = p.init.get();
            ps.loc = n.loc;
            out.push_back(std::move(ps));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Bodies
// ---------------------------------------------------------------------------

ir::ModuleId Elaborator::bodyFor(const DesignUnit& entity, const DesignUnit& arch, const std::string& lib,
                                 Scope* entityScope, const std::vector<PortSpec>& ports, const std::string& canon,
                                 const std::string& params, Loc instLoc) {
    std::string key = canon;
    for (const auto& p : ports) {
        if (p.type && p.type->isArray() && !p.type->indexRanges.empty() && p.type->indexRanges[0]) {
            key += "|" + p.key + ":" + std::to_string(p.type->indexRanges[0]->left) +
                   (p.type->indexRanges[0]->ascending ? "to" : "downto") +
                   std::to_string(p.type->indexRanges[0]->right);
        }
    }
    const BodyKey bk{lib, entity.name.key, arch.name.key, key};
    if (auto it = bodies_.find(bk); it != bodies_.end())
        return it->second;
    if (depth_ > 256) {
        error("VHDL-DEPTH", "instance hierarchy too deep (> 256 levels; recursive instantiation?)", instLoc);
        return {};
    }
    ++depth_;
    BodyState bs;
    bs.body.name = entity.name.text;
    bs.body.params = params;
    bs.body.loc = srcLoc(entity.loc);
    bs.entityKey = entity.name.key;
    bs.ed = std::make_unique<ir::BodyEditor>(bs.body, "process");
    BodyState* saved = body;
    std::vector<Object> savedFrames;
    savedFrames.swap(frameObjects);
    body = &bs;

    std::vector<PortPiece> pieces;
    Ctx ectx;
    ectx.scope = entityScope;
    for (std::size_t k = 0; k < ports.size(); ++k) {
        const PortSpec& p = ports[k];
        if (!p.type)
            continue;
        if (!p.type->isFullyConstrained()) {
            error("VHDL-UNCONSTRAINED", "port '" + p.display + "' of '" + entity.name.text +
                                            "' is unconstrained and not associated with a constrained actual",
                  p.loc);
            continue;
        }
        SVal init;
        if (p.defaultExpr && p.mode != Mode::In) {
            Ctx c = ectx;
            c.constOnly = true;
            init = eval(*p.defaultExpr, c, p.type);
        }
        Symbol* s = newSymbol(SymKind::Signal, p.key, p.display, p.loc);
        s->type = p.type;
        s->object = createStorageObject(p.display, p.type, true, p.mode, p.loc, {}, init.valid() ? &init : nullptr);
        entityScope->add(s);
        const Object& o = object(s->object);
        std::uint32_t off = 0;
        for (const auto& ch : o.storage.chunks()) {
            if (!ch.isConst()) {
                bs.body.ports.push_back({bs.body.net(ch.net).name, dirOf(p.mode), ch.net});
                pieces.push_back({k, off, ch.width});
            }
            off += ch.width;
        }
    }
    elaborateDeclarativePart(*entityScope, entity.decls, ectx);

    Scope* as = newScope(entityScope, entity.name.text + "(" + arch.name.text + ")");
    as->workLibrary = lib;
    applyContext(*as, arch.context, lib);
    Ctx actx;
    actx.scope = as;
    elaborateDeclarativePart(*as, arch.decls, actx);
    elaborateConcurrent(arch.stmts, *as);

    // inputs must not be driven inside
    bs.ed->finish(sink_, [](const std::vector<std::uint32_t>&) {});
    body = saved;
    frameObjects.swap(savedFrames);
    --depth_;
    const ir::ModuleId id = design_->addBody(std::move(bs.body));
    bodies_.emplace(bk, id);
    portPieces_[id.index()] = std::move(pieces);
    portSpecs_[id.index()] = ports;
    return id;
}

// ---------------------------------------------------------------------------
// Concurrent statements
// ---------------------------------------------------------------------------

void Elaborator::elaborateConcurrent(const std::vector<StmtPtr>& stmts, Scope& scope) {
    for (const auto& sp : stmts) {
        const Stmt& s = *sp;
        switch (s.kind) {
            case StmtKind::Process: lowerProcess(s, scope); break;
            case StmtKind::ConcAssign: lowerConcurrentAssign(s, scope); break;
            case StmtKind::ConcProcCall:
                unsupported("IR-UNSUPPORTED-STATEMENT", "concurrent procedure call", s.loc);
                break;
            case StmtKind::Instance: elaborateInstance(s, scope); break;
            case StmtKind::Block: {
                Scope* bsc = newScope(&scope, s.label.text);
                Ctx c;
                c.scope = bsc;
                if (!s.generics.empty()) {
                    // block generics: constants bound to the generic map
                    std::size_t pos = 0;
                    for (const auto& g : s.generics) {
                        declareInterface(*bsc, g, c);
                        for (const auto& n : g.names) {
                            const Expr* actual = nullptr;
                            for (const auto& a : s.genericMap) {
                                const Expr* b = formalBase(a.formal.get());
                                if (b && b->ident.key == n.key)
                                    actual = a.actual.get();
                            }
                            if (!actual && pos < s.genericMap.size() && !s.genericMap[pos].formal)
                                actual = s.genericMap[pos].actual.get();
                            ++pos;
                            if (actual) {
                                Ctx pc;
                                pc.scope = &scope;
                                Symbol* gsym = bsc->local(n.key)->back();
                                gsym->value = eval(*actual, pc, gsym->type);
                            }
                        }
                    }
                }
                if (!s.ports.empty())
                    unsupported("IR-UNSUPPORTED-BLOCK", "block '" + s.label.text + "' with ports", s.loc);
                const std::string savedPrefix = body->prefix;
                body->prefix += s.label.text + ".";
                elaborateDeclarativePart(*bsc, s.decls, c);
                elaborateConcurrent(s.body, *bsc);
                body->prefix = savedPrefix;
                break;
            }
            case StmtKind::ForGenerate: {
                Ctx c;
                c.scope = &scope;
                const Type* it = nullptr;
                auto r = discreteRange(*s.range, c, &it);
                if (!r)
                    break;
                if (r->length() > opts_.maxLoopIterations) {
                    error("VHDL-GENERATE", "generate '" + s.label.text + "' has too many iterations", s.loc);
                    break;
                }
                const std::string savedPrefix = body->prefix;
                for (std::uint64_t k = 0; k < r->length(); ++k) {
                    const std::int64_t v = r->indexAt(k);
                    Scope* gsc = newScope(&scope, s.label.text);
                    Symbol* var = newSymbol(SymKind::Constant, s.loopVar.key, s.loopVar.text, s.loopVar.loc);
                    var->type = it ? it : std_.integer;
                    var->value = makeConst(var->type, Value::integer(v));
                    gsc->add(var);
                    std::string idx = std::to_string(v);
                    if (var->type->kind == TypeKind::Enum && var->type->enumInfo &&
                        static_cast<std::size_t>(v) < var->type->enumInfo->display.size())
                        idx = var->type->enumInfo->display[static_cast<std::size_t>(v)];
                    body->prefix = savedPrefix + s.label.text + "[" + idx + "].";
                    Ctx gc;
                    gc.scope = gsc;
                    elaborateDeclarativePart(*gsc, s.decls, gc);
                    elaborateConcurrent(s.body, *gsc);
                }
                body->prefix = savedPrefix;
                break;
            }
            case StmtKind::IfGenerate: {
                Ctx c;
                c.scope = &scope;
                for (const auto& b : s.branches) {
                    bool take = !b.cond;
                    if (b.cond) {
                        SVal v = eval(*b.cond, c, std_.boolean);
                        if (!v.valid())
                            break;
                        if (!v.known()) {
                            error("VHDL-GENERATE", "if-generate condition is not static", b.loc);
                            break;
                        }
                        take = v.value->i != 0;
                    }
                    if (!take)
                        continue;
                    Scope* gsc = newScope(&scope, s.label.text);
                    const std::string savedPrefix = body->prefix;
                    body->prefix += s.label.text + ".";
                    Ctx gc;
                    gc.scope = gsc;
                    elaborateDeclarativePart(*gsc, b.decls, gc);
                    elaborateConcurrent(b.body, *gsc);
                    body->prefix = savedPrefix;
                    break;
                }
                break;
            }
            case StmtKind::CaseGenerate: {
                Ctx c;
                c.scope = &scope;
                SVal sel = eval(*s.selector, c, nullptr);
                if (!sel.valid())
                    break;
                if (!sel.known()) {
                    error("VHDL-GENERATE", "case-generate expression is not static", s.loc);
                    break;
                }
                const CaseAlt* chosen = nullptr;
                for (const auto& a : s.caseAlts) {
                    for (const auto& ch : a.choices) {
                        if (ch->kind == ExprKind::Others) {
                            chosen = &a;
                            break;
                        }
                        if (ch->kind == ExprKind::Range) {
                            auto r = discreteRange(*ch, c);
                            if (r && sel.value->kind == Value::Kind::Int && r->contains(sel.value->i)) {
                                chosen = &a;
                                break;
                            }
                            continue;
                        }
                        SVal cv = eval(*ch, c, sel.type);
                        if (cv.known() && convert(cv, sel.type, c, ch->loc).value == sel.value) {
                            chosen = &a;
                            break;
                        }
                    }
                    if (chosen)
                        break;
                }
                if (!chosen)
                    break;
                Scope* gsc = newScope(&scope, s.label.text);
                const std::string savedPrefix = body->prefix;
                body->prefix += s.label.text + ".";
                Ctx gc;
                gc.scope = gsc;
                elaborateDeclarativePart(*gsc, chosen->decls, gc);
                elaborateConcurrent(chosen->body, *gsc);
                body->prefix = savedPrefix;
                break;
            }
            case StmtKind::ConcAssert:
            case StmtKind::Ignored:
            case StmtKind::Null: break;
            default: error("VHDL-STATEMENT", "sequential statement in a concurrent region", s.loc); break;
        }
    }
}

// ---------------------------------------------------------------------------
// Instances
// ---------------------------------------------------------------------------

namespace {

struct FormalPart {
    std::uint32_t offset = 0;
    std::uint32_t width = 0;
    const Type* type = nullptr;
};

} // namespace

void Elaborator::elaborateInstance(const Stmt& s, Scope& scope) {
    const std::string instName = body->prefix + s.label.text;
    Ctx pctx;
    pctx.scope = &scope;

    // ---- what is instantiated ----------------------------------------
    std::string libName = scope.workLibrary;
    std::string unitKey;
    std::string unitText;
    const Expr* u = s.unit.get();
    if (u && u->kind == ExprKind::Selected) {
        if (u->prefix->kind == ExprKind::Simple) {
            libName = u->prefix->ident.key == "work" ? scope.workLibrary : u->prefix->ident.key;
        }
        unitKey = u->ident.key;
        unitText = u->ident.text;
    }
    else if (u) {
        unitKey = u->ident.key;
        unitText = u->ident.text;
    }
    const Decl* component = nullptr;
    const DesignUnit* entity = nullptr;
    const DesignUnit* arch = nullptr;
    std::string entityLib = libName;
    switch (s.unitKind) {
        case Stmt::UnitKind::Entity: {
            LibraryUnits* l = library(libName);
            if (l)
                if (auto it = l->entities.find(unitKey); it != l->entities.end())
                    entity = it->second;
            break;
        }
        case Stmt::UnitKind::Component: {
            if (u) {
                auto syms = resolveName(*u, pctx, true);
                for (Symbol* sym : syms)
                    if (sym->kind == SymKind::Component)
                        component = sym->component;
            }
            if (!component)
                warn("VHDL-COMPONENT", "component '" + unitText + "' is not declared; binding by name", s.loc);
            entity = findEntity(scope.workLibrary, unitKey, &entityLib);
            break;
        }
        case Stmt::UnitKind::Configuration: {
            LibraryUnits* l = library(libName);
            if (l)
                if (auto it = l->configurations.find(unitKey); it != l->configurations.end()) {
                    warn("VHDL-CONFIGURATION",
                         "configuration '" + unitText + "': its bindings are ignored, default binding is used", s.loc);
                    entity = findEntity(libName, it->second->entity.key, &entityLib);
                }
            break;
        }
    }
    if (entity) {
        if (!s.architecture.empty()) {
            if (LibraryUnits* l = library(entityLib))
                if (auto it = l->architectures.find(entity->name.key); it != l->architectures.end())
                    for (const DesignUnit* a : it->second)
                        if (a->name.key == s.architecture.key)
                            arch = a;
            if (!arch)
                error("VHDL-ARCHITECTURE", "entity '" + entity->name.text + "' has no architecture '" +
                                               s.architecture.text + "'",
                      s.loc);
        }
        else {
            arch = defaultArchitecture(entityLib, *entity);
            if (!arch)
                warn("VHDL-ARCHITECTURE", "entity '" + entity->name.text + "' has no architecture", s.loc);
        }
    }
    if (!entity && opts_.resolveForeign) {
        if (auto it = opts_.foreignModules.find(unitKey); it != opts_.foreignModules.end()) {
            elaborateForeignInstance(s, scope, it->second);
            return;
        }
    }
    if (!entity || !arch) {
        ir::ChildInstance ci;
        ci.name = instName;
        ci.definition = unitText.empty() ? "?" : unitText;
        ci.loc = srcLoc(s.loc);
        body->body.children.push_back(std::move(ci));
        if (design_)
            design_->unsupported.push_back({"IR-UNRESOLVED-INSTANCE", body->body.name,
                                            "instance '" + instName + "' of unknown entity/component '" + unitText +
                                                "'",
                                            srcLoc(s.loc)});
        warn("IR-UNRESOLVED-INSTANCE",
             "instance '" + instName + "' of unknown entity/component '" + unitText + "' (treated as a black box)",
             s.loc);
        return;
    }

    // ---- generics ------------------------------------------------------
    std::string canon;
    std::string params;
    Scope* gs = bindGenerics(*entity, entityLib, &s.genericMap, component, &pctx, s.loc, canon, params);
    if (!gs)
        return;
    std::vector<PortSpec> specs = portSpecs(*entity, gs);

    // ---- port associations ---------------------------------------------
    struct Assoc1 {
        std::size_t port = 0;
        const Expr* formal = nullptr; ///< partial formal (null: whole port)
        const Expr* actual = nullptr;
        Loc loc;
    };
    std::vector<Assoc1> assocs;
    std::vector<std::string> positional;
    if (component) {
        for (const auto& p : component->ports)
            for (const auto& n : p.names)
                positional.push_back(n.key);
    }
    else {
        for (const auto& p : specs)
            positional.push_back(p.key);
    }
    auto specIndex = [&](const std::string& key) -> std::optional<std::size_t> {
        for (std::size_t k = 0; k < specs.size(); ++k)
            if (specs[k].key == key)
                return k;
        return std::nullopt;
    };
    std::size_t pos = 0;
    for (const auto& a : s.portMap) {
        std::string key;
        const Expr* partial = nullptr;
        if (!a.formal) {
            if (pos >= positional.size()) {
                error("VHDL-PORT-MAP", "too many ports in the port map of '" + instName + "'", a.loc);
                continue;
            }
            key = positional[pos++];
        }
        else {
            const Expr* b = formalBase(a.formal.get());
            if (!b) {
                unsupported("IR-UNSUPPORTED-PORT", "port association with this formal", a.loc);
                continue;
            }
            key = b->ident.key;
            if (b != a.formal.get())
                partial = a.formal.get();
        }
        auto k = specIndex(key);
        if (!k) {
            // formal in a conversion function: f(port) => actual
            if (a.formal && a.formal->kind == ExprKind::Call && a.formal->args.size() == 1 &&
                a.formal->args[0].actual && a.formal->args[0].actual->kind == ExprKind::Simple &&
                specIndex(a.formal->args[0].actual->ident.key)) {
                unsupported("IR-UNSUPPORTED-PORT", "conversion function in the formal part of a port association",
                            a.loc);
                continue;
            }
            error("VHDL-PORT-MAP", "entity '" + entity->name.text + "' has no port '" + key + "'", a.loc);
            continue;
        }
        if (!a.actual || a.actual->kind == ExprKind::Open)
            continue;
        const Expr* actual = a.actual.get();
        if (actual->kind == ExprKind::Inertial)
            actual = actual->lhs.get();
        assocs.push_back({*k, partial, actual, a.loc});
    }
    // unconstrained formals take the actual's constraint
    for (auto& as : assocs) {
        PortSpec& p = specs[as.port];
        if (as.formal || !p.type || p.type->isFullyConstrained())
            continue;
        const Type* at = nullptr;
        if (p.mode == Mode::In) {
            TypeSet ts = typesOf(*as.actual, pctx);
            if (ts.types.size() == 1)
                at = ts.types.front();
            if (!at || !at->isFullyConstrained()) {
                SVal v = eval(*as.actual, pctx, p.type);
                at = v.type;
            }
        }
        else {
            std::vector<Target> tg;
            if (!targets(*as.actual, pctx, tg, &at))
                at = nullptr;
        }
        if (at && at->isArray() && p.type->isArray() && at->isFullyConstrained()) {
            std::vector<IntRange> rs;
            for (const auto& r : at->indexRanges)
                rs.push_back(*r);
            p.type = types.constrainedArray(p.type, rs);
        }
    }

    const ir::ModuleId cid = bodyFor(*entity, *arch, entityLib, gs, specs, canon, params, s.loc);
    if (!cid.valid())
        return;

    // ---- actual bits per formal ----------------------------------------
    std::vector<SigSpec> formalBits(specs.size());
    std::vector<BitMask> have(specs.size());
    std::vector<bool> bad(specs.size(), false);
    for (std::size_t k = 0; k < specs.size(); ++k) {
        const std::uint32_t W = specs[k].type ? bitWidth(*specs[k].type).value_or(0) : 0;
        formalBits[k] = SigSpec::allX(W);
        have[k].assign(W, false);
    }
    std::function<std::optional<FormalPart>(const Expr&, const PortSpec&)> part =
        [&](const Expr& e, const PortSpec& p) -> std::optional<FormalPart> {
        if (e.kind == ExprKind::Simple)
            return FormalPart{0, bitWidth(*p.type).value_or(0), p.type};
        if (e.kind == ExprKind::Selected) {
            auto b = part(*e.prefix, p);
            if (!b || b->type->kind != TypeKind::Record)
                return std::nullopt;
            std::uint32_t off = 0;
            for (std::size_t f = b->type->fields.size(); f-- > 0;) {
                const auto fw = bitWidth(*b->type->fields[f].type).value_or(0);
                if (b->type->fields[f].key == e.ident.key)
                    return FormalPart{b->offset + off, fw, b->type->fields[f].type};
                off += fw;
            }
            return std::nullopt;
        }
        if (e.kind == ExprKind::Call && e.args.size() == 1 && e.args[0].actual) {
            auto b = part(*e.prefix, p);
            if (!b || !b->type->isArray() || b->type->indexRanges.size() != 1 || !b->type->indexRanges[0])
                return std::nullopt;
            const IntRange& r = *b->type->indexRanges[0];
            const std::uint32_t ew = bitWidth(*b->type->element).value_or(0);
            const std::uint64_t len = r.length();
            const Expr& ix = *e.args[0].actual;
            if (ix.kind == ExprKind::Range || ix.kind == ExprKind::Attribute || ix.kind == ExprKind::Subtype) {
                auto sr = discreteRange(ix, pctx);
                if (!sr || sr->isNull() || !r.contains(sr->low()) || !r.contains(sr->high()))
                    return std::nullopt;
                const std::uint64_t p1 = r.positionOf(sr->left);
                const std::uint64_t p2 = r.positionOf(sr->right);
                const std::uint64_t hiPos = std::max(p1, p2);
                const std::uint64_t n = sr->length();
                return FormalPart{b->offset + static_cast<std::uint32_t>((len - 1 - hiPos) * ew),
                                  static_cast<std::uint32_t>(n * ew), types.constrainedArray(b->type, {*sr})};
            }
            auto v = constInt(ix, pctx);
            if (!v || !r.contains(*v))
                return std::nullopt;
            const std::uint64_t ps = r.positionOf(*v);
            return FormalPart{b->offset + static_cast<std::uint32_t>((len - 1 - ps) * ew), ew, b->type->element};
        }
        return std::nullopt;
    };
    for (const auto& as : assocs) {
        const PortSpec& p = specs[as.port];
        if (!p.type || !bitWidth(*p.type))
            continue;
        FormalPart fp{0, bitWidth(*p.type).value_or(0), p.type};
        if (as.formal) {
            auto r = part(*as.formal, p);
            if (!r) {
                unsupported("IR-UNSUPPORTED-PORT", "partial association of port '" + p.display + "'", as.loc);
                bad[as.port] = true;
                continue;
            }
            fp = *r;
        }
        SigSpec bits;
        if (p.mode == Mode::In) {
            SVal v = eval(*as.actual, pctx, fp.type);
            if (!v.valid()) {
                bad[as.port] = true;
                continue;
            }
            v = convert(v, fp.type, pctx, as.loc);
            if (!v.valid()) {
                bad[as.port] = true;
                continue;
            }
            bits = bitsOf(v, as.loc);
        }
        else {
            std::vector<Target> tg;
            const Type* tt = nullptr;
            if (!targets(*as.actual, pctx, tg, &tt)) {
                bad[as.port] = true;
                continue;
            }
            bool ok = true;
            for (auto it = tg.rbegin(); it != tg.rend(); ++it) {
                if (it->dynamic) {
                    ok = false;
                    break;
                }
                const Object& o = object(it->object);
                if (!o.isSignal || o.storage.empty()) {
                    ok = false;
                    break;
                }
                bits.append(o.storage.extract(it->offset, it->width));
            }
            if (!ok) {
                unsupported("IR-UNSUPPORTED-PORT",
                            "output port '" + p.display + "' of '" + instName + "' connected to an unsupported expression",
                            as.loc);
                bad[as.port] = true;
                continue;
            }
        }
        if (bits.width() != fp.width) {
            error("VHDL-LENGTH", "port '" + p.display + "' has " + std::to_string(fp.width) +
                                     " bit(s), its actual has " + std::to_string(bits.width()),
                  as.loc);
            bad[as.port] = true;
            continue;
        }
        formalBits[as.port] = formalBits[as.port].replaced(fp.offset, bits);
        for (std::uint32_t i = 0; i < fp.width; ++i)
            have[as.port][fp.offset + i] = true;
    }
    // unconnected inputs with a default value
    for (std::size_t k = 0; k < specs.size(); ++k) {
        const PortSpec& p = specs[k];
        if (p.mode != Mode::In || !p.defaultExpr || bad[k] || !p.type)
            continue;
        if (std::ranges::any_of(have[k], [](bool b) { return b; }))
            continue;
        Ctx c;
        c.scope = gs;
        c.constOnly = true;
        SVal v = eval(*p.defaultExpr, c, p.type);
        if (!v.valid())
            continue;
        v = convert(v, p.type, c, p.loc);
        SigSpec bits = bitsOf(v, p.loc);
        if (bits.width() == formalBits[k].width()) {
            formalBits[k] = bits;
            std::fill(have[k].begin(), have[k].end(), true);
        }
    }

    ir::ChildInstance ci;
    ci.name = instName;
    ci.definition = entity->name.text;
    ci.body = cid;
    ci.loc = srcLoc(s.loc);
    const auto& childPorts = design_->body(cid).ports;
    const auto& pieces = portPieces_[cid.index()];
    for (std::size_t k = 0; k < childPorts.size() && k < pieces.size(); ++k) {
        const PortPiece& pc = pieces[k];
        ir::PortBinding pb;
        pb.port = childPorts[k].name;
        pb.dir = childPorts[k].dir;
        pb.childNet = childPorts[k].net;
        if (pc.port < specs.size() && !bad[pc.port]) {
            const BitMask& h = have[pc.port];
            bool all = true;
            bool none = true;
            for (std::uint32_t i = pc.offset; i < pc.offset + pc.width && i < h.size(); ++i) {
                all = all && h[i];
                none = none && !h[i];
            }
            if (all || (!none && pb.dir == ir::PortDir::In))
                pb.actual = formalBits[pc.port].extract(pc.offset, pc.width);
            else if (!none)
                unsupported("IR-UNSUPPORTED-PORT", "output port '" + pb.port + "' of '" + instName +
                                                       "' is only partially connected",
                            s.loc);
        }
        ci.ports.push_back(std::move(pb));
    }
    body->body.children.push_back(std::move(ci));
}

// ---------------------------------------------------------------------------
// Top level
// ---------------------------------------------------------------------------

namespace {

void collectInstantiated(const std::vector<StmtPtr>& stmts, std::set<std::string>& out) {
    for (const auto& s : stmts) {
        if (s->kind == StmtKind::Instance && s->unit) {
            out.insert(s->unit->ident.key);
        }
        collectInstantiated(s->body, out);
        for (const auto& b : s->branches)
            collectInstantiated(b.body, out);
        for (const auto& a : s->caseAlts)
            collectInstantiated(a.body, out);
    }
}

frontend::InstanceInfo makeInfo(const ir::Design& d, ir::InstanceId id) {
    const auto& n = d.instance(id);
    frontend::InstanceInfo info;
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

std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

std::set<std::string> Elaborator::instantiatedNames() const {
    std::set<std::string> out;
    for (const auto& [ln, lib] : libraries)
        for (const auto& [k, archs] : lib.architectures)
            for (const DesignUnit* a : archs)
                collectInstantiated(a->stmts, out);
    return out;
}

std::vector<frontend::DefinitionInfo> Elaborator::definitions() const {
    std::vector<frontend::DefinitionInfo> out;
    for (const auto& [ln, lib] : libraries)
        for (const auto& [k, e] : lib.entities)
            out.push_back({e->name.text, "entity", srcLoc(e->loc)});
    std::ranges::sort(out, [](const auto& a, const auto& b) { return a.name < b.name; });
    return out;
}

std::shared_ptr<ir::Design> Elaborator::run(frontend::ElaborationSummary& summary) {
    design_ = opts_.design ? opts_.design : std::make_shared<ir::Design>();
    summary.definitions = definitions();
    for (const ir::ModuleId id : elaborateTops())
        design_->addTop(id);
    for (auto top : design_->tops)
        summary.topInstances.push_back(makeInfo(*design_, top));
    summary.instanceCount = design_->instances.size();
    return design_;
}

std::vector<ir::ModuleId> Elaborator::elaborateTops() {
    if (!design_)
        design_ = opts_.design ? opts_.design : std::make_shared<ir::Design>();
    std::vector<ir::ModuleId> result;
    // top selection
    struct Top {
        std::string lib;
        const DesignUnit* entity = nullptr;
        const DesignUnit* arch = nullptr;
    };
    std::vector<Top> tops;
    if (!opts_.top.empty()) {
        std::string t = lower(opts_.top);
        std::string archName;
        if (auto p = t.find('('); p != std::string::npos && t.back() == ')') {
            archName = t.substr(p + 1, t.size() - p - 2);
            t = t.substr(0, p);
        }
        std::string libName;
        if (auto p = t.find('.'); p != std::string::npos) {
            libName = t.substr(0, p);
            t = t.substr(p + 1);
        }
        std::string found;
        const DesignUnit* e = findEntity(libName.empty() ? "work" : libName, t, &found);
        if (!e || (!libName.empty() && found != libName)) {
            sink_.report({"VHDL-TOP", Severity::Error, "top entity '" + opts_.top + "' not found", {}, {}});
            ++errors_;
            return result;
        }
        const DesignUnit* a = nullptr;
        if (!archName.empty()) {
            if (LibraryUnits* l = library(found))
                if (auto it = l->architectures.find(e->name.key); it != l->architectures.end())
                    for (const DesignUnit* x : it->second)
                        if (x->name.key == archName)
                            a = x;
        }
        else {
            a = defaultArchitecture(found, *e);
        }
        if (!a) {
            sink_.report({"VHDL-TOP", Severity::Error, "top entity '" + opts_.top + "' has no (such) architecture",
                          srcLoc(e->loc), {}});
            ++errors_;
            return result;
        }
        tops.push_back({found, e, a});
    }
    else {
        std::set<std::string> instantiated;
        for (const auto& [ln, lib] : libraries)
            for (const auto& [k, archs] : lib.architectures)
                for (const DesignUnit* a : archs)
                    collectInstantiated(a->stmts, instantiated);
        for (const auto& [ln, lib] : libraries) {
            for (const auto& [k, e] : lib.entities) {
                if (instantiated.contains(k) || opts_.excludeTops.contains(k))
                    continue;
                const DesignUnit* a = defaultArchitecture(ln, *e);
                if (!a)
                    continue;
                bool generic = false;
                for (const auto& g : e->generics)
                    if (!g.init && g.cls != InterfaceDecl::Class::Type)
                        generic = true;
                if (generic) {
                    warn("VHDL-TOP", "entity '" + e->name.text +
                                         "' is not instantiated but has generics without defaults; not elaborated as a top",
                         e->loc, Severity::Info);
                    continue;
                }
                tops.push_back({ln, e, a});
            }
        }
    }

    for (const auto& t : tops) {
        std::string canon;
        std::string params;
        Scope* gs = bindGenerics(*t.entity, t.lib, nullptr, nullptr, nullptr, t.entity->loc, canon, params);
        if (!gs)
            continue;
        auto specs = portSpecs(*t.entity, gs);
        const ir::ModuleId id = bodyFor(*t.entity, *t.arch, t.lib, gs, specs, canon, params, t.entity->loc);
        if (id.valid())
            result.push_back(id);
    }
    return result;
}

} // namespace opencdc::vhdl
