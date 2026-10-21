// VHDL semantic analysis: libraries, scopes, visibility, packages,
// declarations and type definitions.

#include "frontend/vhdl/VhdlElab.h"
#include "frontend/vhdl/VhdlLexer.h"

#include <algorithm>
#include <cmath>

namespace opencdc::vhdl {

namespace {

bool overloadable(const Symbol* s) {
    return s->kind == SymKind::Subprogram || s->kind == SymKind::EnumLiteral;
}

} // namespace

void Scope::add(Symbol* s) {
    auto [it, inserted] = names.try_emplace(s->key);
    if (inserted)
        order.push_back(s->key);
    it->second.push_back(s);
}

const std::vector<Symbol*>* Scope::local(const std::string& key) const {
    auto it = names.find(key);
    return it == names.end() ? nullptr : &it->second;
}

Symbol* Elaborator::newSymbol(SymKind k, const std::string& key, const std::string& display, Loc l) {
    Symbol& s = symbols.emplace_back();
    s.kind = k;
    s.key = key;
    s.display = display;
    s.loc = l;
    return &s;
}

Scope* Elaborator::newScope(Scope* parent, std::string label) {
    Scope& s = scopes.emplace_back();
    s.parent = parent;
    s.label = std::move(label);
    if (parent)
        s.workLibrary = parent->workLibrary;
    return &s;
}

// ---------------------------------------------------------------------------
// Libraries
// ---------------------------------------------------------------------------

LibraryUnits* Elaborator::library(const std::string& name) {
    auto it = libraries.find(name);
    return it == libraries.end() ? nullptr : &it->second;
}

void Elaborator::indexLibraries() {
    for (auto& f : files_) {
        auto& lib = libraries[f.library];
        for (const auto& u : f.ast.units) {
            const std::string& key = u.name.key;
            auto dup = [&](const std::map<std::string, const DesignUnit*>& m, const char* what) {
                if (auto it = m.find(key); it != m.end())
                    warn("VHDL-REDEFINED",
                         std::string(what) + " '" + u.name.text + "' is defined again; the later definition is used",
                         u.loc);
            };
            switch (u.kind) {
                case UnitKind::Entity:
                    dup(lib.entities, "entity");
                    lib.entities[key] = &u;
                    break;
                case UnitKind::Architecture: lib.architectures[u.entity.key].push_back(&u); break;
                case UnitKind::Package:
                case UnitKind::PackageInstance:
                    dup(lib.packages, "package");
                    lib.packages[key] = &u;
                    break;
                case UnitKind::PackageBody: lib.packageBodies[key] = &u; break;
                case UnitKind::Configuration: lib.configurations[key] = &u; break;
                case UnitKind::Context: lib.contexts[key] = &u; break;
            }
        }
    }
    for (auto& [name, lib] : libraries) {
        Symbol* s = newSymbol(SymKind::Library, name, name, {});
        s->library = name;
        root->add(s);
    }
}

PackageInfo* Elaborator::findPackage(const std::string& libName, const std::string& name, Loc l) {
    const std::pair<std::string, std::string> key{libName, name};
    if (auto it = packages.find(key); it != packages.end())
        return it->second.get();
    if (auto* b = builtinPackage(libName, name))
        return b;
    LibraryUnits* lib = library(libName);
    if (!lib || !lib->packages.contains(name)) {
        error("VHDL-UNKNOWN-PACKAGE", "package '" + libName + "." + name + "' not found", l);
        return nullptr;
    }
    auto p = std::make_unique<PackageInfo>();
    p->library = libName;
    p->name = name;
    p->decl = lib->packages[name];
    if (auto it = lib->packageBodies.find(name); it != lib->packageBodies.end())
        p->body = it->second;
    PackageInfo* raw = p.get();
    packages.emplace(key, std::move(p));
    elaboratePackage(*raw);
    return raw;
}

void Elaborator::elaboratePackage(PackageInfo& p) {
    if (p.elaborated || p.inProgress)
        return;
    p.inProgress = true;
    p.scope.parent = root;
    p.scope.workLibrary = p.library;
    p.scope.label = p.library + "." + p.name;
    if (p.decl) {
        applyContext(p.scope, p.decl->context, p.library);
        Ctx ctx;
        ctx.scope = &p.scope;
        ctx.constOnly = true;
        if (p.decl->kind == UnitKind::PackageInstance) {
            // package p is new lib.generic_pkg generic map (...)
            const auto syms = resolveName(*p.decl->instantiated, ctx);
            PackageInfo* gen = !syms.empty() && syms.front()->kind == SymKind::Package ? syms.front()->package : nullptr;
            if (!gen || !gen->decl) {
                error("VHDL-PACKAGE-INST", "cannot instantiate package '" + p.name + "'", p.decl->loc);
            }
            else {
                applyContext(p.scope, gen->decl->context, gen->library);
                // generics bound to the generic map actuals
                std::size_t pos = 0;
                for (const auto& g : gen->decl->generics) {
                    declareInterface(p.scope, g, ctx);
                    for (const auto& n : g.names) {
                        const Expr* actual = nullptr;
                        for (const auto& a : p.decl->genericMap) {
                            if (a.formal && a.formal->kind == ExprKind::Simple && a.formal->ident.key == n.key)
                                actual = a.actual.get();
                        }
                        if (!actual && pos < p.decl->genericMap.size() && !p.decl->genericMap[pos].formal)
                            actual = p.decl->genericMap[pos].actual.get();
                        ++pos;
                        auto* v = p.scope.local(n.key);
                        if (v && actual && actual->kind != ExprKind::Open && actual->kind != ExprKind::Box) {
                            Symbol* gs = v->back();
                            gs->value = eval(*actual, ctx, gs->type);
                        }
                    }
                }
                for (const auto& d : gen->decl->decls)
                    declare(p.scope, *d, ctx);
                if (auto* lib = library(gen->library))
                    if (auto it = lib->packageBodies.find(gen->name); it != lib->packageBodies.end())
                        p.body = it->second;
            }
        }
        else if (!p.decl->generics.empty()) {
            // uninstantiated generic package: only a template for "package ... is new"
            p.inProgress = false;
            p.elaborated = true;
            return;
        }
        else {
            for (const auto& d : p.decl->decls)
                declare(p.scope, *d, ctx);
        }
    }
    p.inProgress = false;
    p.elaborated = true;
    // Deferred constants need the body; elaborate it eagerly (cheap: only
    // declarations, subprogram bodies are bound, not executed).
    if (p.body)
        elaboratePackageBody(p);
}

void Elaborator::elaboratePackageBody(PackageInfo& p) {
    if (p.bodyElaborated || !p.body)
        return;
    p.bodyElaborated = true;
    p.bodyScope.parent = &p.scope;
    p.bodyScope.workLibrary = p.library;
    p.bodyScope.label = p.scope.label + " body";
    applyContext(p.bodyScope, p.body->context, p.library);
    Ctx ctx;
    ctx.scope = &p.bodyScope;
    ctx.constOnly = true;
    for (const auto& d : p.body->decls)
        declare(p.bodyScope, *d, ctx);
}

void Elaborator::applyContext(Scope& scope, const std::vector<ContextItem>& items, const std::string& workLib) {
    scope.workLibrary = workLib;
    for (const auto& item : items) {
        switch (item.kind) {
            case ContextItem::Kind::Library:
                for (const auto& l : item.libraries)
                    scope.libraries.push_back(l.key);
                break;
            case ContextItem::Kind::Use: {
                Decl d;
                d.kind = DeclKind::Use;
                d.loc = item.loc;
                for (const auto& n : item.names) {
                    // borrow: Decl owns ExprPtr; build a non-owning use by re-resolving here
                    Ctx ctx;
                    ctx.scope = &scope;
                    ctx.constOnly = true;
                    const Expr& name = *n;
                    if (name.kind != ExprKind::Selected) {
                        error("VHDL-USE", "use clause needs a selected name", item.loc);
                        continue;
                    }
                    if (name.ident.key == "all") {
                        auto syms = resolveName(*name.prefix, ctx);
                        if (syms.empty())
                            continue;
                        if (syms.front()->kind == SymKind::Package) {
                            scope.usedAll.push_back(&syms.front()->package->scope);
                        }
                        else if (syms.front()->kind == SymKind::Library) {
                            // use lib.all: makes all units of the library visible (by name)
                            scope.libraries.push_back(syms.front()->library);
                        }
                        else {
                            warn("VHDL-USE", "'use " + name.prefix->ident.text + ".all' does not name a package",
                                 item.loc);
                        }
                    }
                    else {
                        auto pre = resolveName(*name.prefix, ctx);
                        if (pre.empty())
                            continue;
                        if (pre.front()->kind == SymKind::Package)
                            scope.usedItems.emplace_back(&pre.front()->package->scope, name.ident.key);
                        else if (pre.front()->kind == SymKind::Library)
                            resolveName(name, ctx); // use lib.pkg; makes pkg visible as a name (ok)
                    }
                }
                break;
            }
            case ContextItem::Kind::Context:
                for (const auto& n : item.names) {
                    // context lib.ctx: apply its items
                    std::string lib = scope.workLibrary;
                    std::string name;
                    if (n->kind == ExprKind::Selected && n->prefix->kind == ExprKind::Simple) {
                        lib = n->prefix->ident.key == "work" ? scope.workLibrary : n->prefix->ident.key;
                        name = n->ident.key;
                    }
                    else if (n->kind == ExprKind::Simple) {
                        name = n->ident.key;
                    }
                    const LibraryUnits* lu = library(lib);
                    if (!lu || !lu->contexts.contains(name)) {
                        error("VHDL-UNKNOWN-CONTEXT", "context '" + lib + "." + name + "' not found", item.loc);
                        continue;
                    }
                    applyContext(scope, lu->contexts.at(name)->context, lib);
                    scope.workLibrary = workLib;
                }
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// Lookup
// ---------------------------------------------------------------------------

std::vector<Symbol*> Elaborator::lookup(Scope& scope, const std::string& key) {
    std::vector<Symbol*> out;
    auto addUnique = [&](Symbol* s) {
        if (std::ranges::find(out, s) == out.end())
            out.push_back(s);
    };
    // Directly visible declarations: the innermost non-overloadable one
    // hides everything outside it.
    for (Scope* s = &scope; s; s = s->parent) {
        if (const auto* v = s->local(key)) {
            for (Symbol* sym : *v) {
                if (!overloadable(sym)) {
                    if (out.empty())
                        return {sym};
                    return out; // overloadables already found in an inner scope hide it
                }
                addUnique(sym);
            }
        }
    }
    // Use-visible declarations.
    std::vector<Symbol*> nonOverloadable;
    auto consider = [&](Symbol* sym) {
        if (overloadable(sym))
            addUnique(sym);
        else if (std::ranges::find(nonOverloadable, sym) == nonOverloadable.end())
            nonOverloadable.push_back(sym);
    };
    for (Scope* s = &scope; s; s = s->parent) {
        for (Scope* used : s->usedAll)
            if (const auto* v = used->local(key))
                for (Symbol* sym : *v)
                    consider(sym);
        for (const auto& [used, item] : s->usedItems)
            if (item == key)
                if (const auto* v = used->local(key))
                    for (Symbol* sym : *v)
                        consider(sym);
    }
    if (standardScope && out.empty() && nonOverloadable.empty()) {
        if (const auto* v = standardScope->local(key))
            for (Symbol* sym : *v)
                consider(sym);
    }
    else if (standardScope) {
        if (const auto* v = standardScope->local(key))
            for (Symbol* sym : *v)
                if (overloadable(sym))
                    addUnique(sym);
    }
    if (!nonOverloadable.empty() && out.empty()) {
        // The same type made visible by two packages (e.g. std_logic_arith
        // and numeric_std both declare "unsigned") is not visible in VHDL;
        // tools commonly take the first one: do the same, with a warning.
        if (nonOverloadable.size() > 1 && !reported_.contains("ambig:" + key)) {
            bool distinct = false;
            for (const Symbol* o : nonOverloadable)
                if (o->type != nonOverloadable.front()->type || o->kind != nonOverloadable.front()->kind)
                    distinct = true;
            if (distinct) {
                reported_.insert("ambig:" + key);
                warn("VHDL-AMBIGUOUS-USE",
                     "'" + nonOverloadable.front()->display +
                         "' is made visible by several use clauses; the first one is used",
                     nonOverloadable.front()->loc);
            }
        }
        return {nonOverloadable.front()};
    }
    // Library names (implicitly visible: work, std; and known libraries).
    if (out.empty()) {
        std::string lib = key == "work" ? scope.workLibrary : key;
        if (const auto* v = root->local(lib))
            return {v->front()};
        if (isBuiltinPackage(lib, "") || lib == "ieee" || lib == "std") {
            Symbol* s = newSymbol(SymKind::Library, lib, lib, {});
            s->library = lib;
            root->add(s);
            return {s};
        }
    }
    return out;
}

std::vector<Symbol*> Elaborator::lookupSelected(const Expr& name, Ctx& ctx, bool quiet) {
    // prefix.suffix where prefix is a library or package
    auto pre = resolveName(*name.prefix, ctx, quiet);
    if (pre.empty())
        return {};
    Symbol* p = pre.front();
    const std::string& key = name.ident.key;
    if (p->kind == SymKind::Library) {
        const std::string lib = p->library;
        // package of the library
        if (isBuiltinPackage(lib, key) || (library(lib) && library(lib)->packages.contains(key))) {
            PackageInfo* pkg = findPackage(lib, key, name.loc);
            if (!pkg)
                return {};
            Symbol* s = newSymbol(SymKind::Package, key, name.ident.text, name.loc);
            s->package = pkg;
            return {s};
        }
        if (library(lib) && library(lib)->entities.contains(key)) {
            Symbol* s = newSymbol(SymKind::Component, key, name.ident.text, name.loc);
            s->library = lib;
            return {s};
        }
        if (!quiet)
            error("VHDL-UNDECLARED", "'" + key + "' not found in library '" + lib + "'", name.loc);
        return {};
    }
    if (p->kind == SymKind::Package) {
        if (const auto* v = p->package->scope.local(key))
            return *v;
        if (!quiet)
            error("VHDL-UNDECLARED", "'" + name.ident.text + "' is not declared in package '" + p->package->name + "'",
                  name.loc);
        return {};
    }
    return {}; // record field etc.: handled by expression evaluation
}

std::vector<Symbol*> Elaborator::resolveName(const Expr& name, Ctx& ctx, bool quiet) {
    switch (name.kind) {
        case ExprKind::Simple: {
            auto r = lookup(*ctx.scope, name.ident.key);
            if (r.empty() && !quiet)
                error("VHDL-UNDECLARED", "'" + name.ident.text + "' is not declared", name.loc);
            return r;
        }
        case ExprKind::Character: {
            auto r = lookup(*ctx.scope, name.ident.key);
            if (r.empty() && !quiet)
                error("VHDL-UNDECLARED", "character literal " + name.ident.text + " is not declared", name.loc);
            return r;
        }
        case ExprKind::Selected: return lookupSelected(name, ctx, quiet);
        default: return {};
    }
}

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

const Type* Elaborator::typeMark(const Expr& mark, Ctx& ctx) {
    if (mark.kind == ExprKind::Attribute) {
        // x'subtype, t'base, x'element (VHDL-2008)
        const std::string& a = mark.ident.key;
        if (a == "base") {
            const Type* t = typeMark(*mark.prefix, ctx);
            return t ? &t->baseType() : nullptr;
        }
        TypeSet ts = typesOf(*mark.prefix, ctx);
        const Type* t = ts.types.size() == 1 ? ts.types.front() : nullptr;
        if (!t) {
            // a type mark prefix
            t = typeMark(*mark.prefix, ctx);
        }
        if (!t) {
            error("VHDL-TYPE", "cannot determine the type of '" + mark.ident.text + "' prefix", mark.loc);
            return nullptr;
        }
        if (a == "subtype")
            return t;
        if (a == "element")
            return t->isArray() ? t->element : nullptr;
        error("VHDL-TYPE", "attribute '" + mark.ident.text + "' does not denote a type", mark.loc);
        return nullptr;
    }
    auto syms = resolveName(mark, ctx);
    for (Symbol* s : syms) {
        if (s->kind == SymKind::Type)
            return s->type;
        if (s->kind == SymKind::Alias && s->type && !s->aliasTarget)
            return s->type;
    }
    if (!syms.empty())
        error("VHDL-TYPE", "'" + syms.front()->display + "' is not a type", mark.loc);
    return nullptr;
}

std::optional<IntRange> Elaborator::discreteRange(const Expr& e, Ctx& ctx, const Type** indexType) {
    auto setType = [&](const Type* t) {
        if (indexType)
            *indexType = t;
    };
    switch (e.kind) {
        case ExprKind::Range: {
            TypeSet ls = typesOf(*e.lhs, ctx);
            TypeSet rs = typesOf(*e.rhs, ctx);
            ls.merge(rs);
            const Type* t = nullptr;
            for (const Type* c : ls.types)
                if (c->isDiscrete() && (!t || t->baseTag() == TypeTag::None))
                    t = c;
            if (!t)
                t = std_.integer;
            auto l = constInt(*e.lhs, ctx, t);
            auto r = constInt(*e.rhs, ctx, t);
            if (!l || !r) {
                error("VHDL-NOT-STATIC", "range bounds must be static (constants or generics)", e.loc);
                return std::nullopt;
            }
            setType(t->kind == TypeKind::UniversalInteger ? std_.integer : t);
            return IntRange{*l, *r, e.dir == RangeDir::To};
        }
        case ExprKind::Subtype: {
            const Type* t = subtype(*e.subtype, ctx);
            if (!t)
                return std::nullopt;
            setType(t);
            if (!t->isDiscrete()) {
                error("VHDL-TYPE", "'" + t->name + "' is not a discrete type", e.loc);
                return std::nullopt;
            }
            return t->range;
        }
        case ExprKind::Attribute: {
            const std::string& a = e.ident.key;
            if (a == "range" || a == "reverse_range") {
                std::uint32_t dim = 1;
                if (!e.args.empty()) {
                    auto d = constInt(*e.args.front().actual, ctx);
                    dim = d ? static_cast<std::uint32_t>(*d) : 1;
                }
                const Type* t = nullptr;
                TypeSet ts = typesOf(*e.prefix, ctx);
                if (ts.types.size() == 1)
                    t = ts.types.front();
                if (!t)
                    t = typeMark(*e.prefix, ctx);
                if (!t)
                    return std::nullopt;
                IntRange r;
                if (t->isArray()) {
                    if (dim == 0 || dim > t->indexRanges.size() || !t->indexRanges[dim - 1]) {
                        error("VHDL-RANGE", "'range of an unconstrained array", e.loc);
                        return std::nullopt;
                    }
                    r = *t->indexRanges[dim - 1];
                    setType(t->indexTypes[dim - 1]);
                }
                else if (t->isDiscrete()) {
                    r = t->range;
                    setType(t);
                }
                else {
                    error("VHDL-RANGE", "'range of a non-array, non-discrete prefix", e.loc);
                    return std::nullopt;
                }
                if (a == "reverse_range")
                    r = IntRange{r.right, r.left, !r.ascending};
                return r;
            }
            break;
        }
        case ExprKind::Simple:
        case ExprKind::Selected: {
            auto syms = resolveName(e, ctx, true);
            if (!syms.empty() && syms.front()->kind == SymKind::Type) {
                const Type* t = syms.front()->type;
                if (t->isDiscrete()) {
                    setType(t);
                    return t->range;
                }
            }
            break;
        }
        default: break;
    }
    error("VHDL-RANGE", "expected a discrete range", e.loc);
    return std::nullopt;
}

const Type* Elaborator::subtype(const SubtypeIndication& s, Ctx& ctx) {
    if (!s.mark)
        return nullptr;
    // Index constraints are written as a call on the type mark:
    // std_logic_vector(7 downto 0), t(open)(3 downto 0).
    std::vector<const Expr*> constraintLists;
    const Expr* mark = s.mark.get();
    while (mark->kind == ExprKind::Call) {
        constraintLists.push_back(mark);
        mark = mark->prefix.get();
    }
    std::reverse(constraintLists.begin(), constraintLists.end());
    const Type* t = typeMark(*mark, ctx);
    if (!t)
        return nullptr;
    bool resolved = s.resolution != nullptr;
    // apply index constraints level by level (array of arrays: element constraint)
    std::function<const Type*(const Type*, std::size_t)> constrain = [&](const Type* at,
                                                                        std::size_t level) -> const Type* {
        if (level >= constraintLists.size())
            return at;
        const Expr* c = constraintLists[level];
        if (!at->isArray()) {
            error("VHDL-TYPE", "index constraint on non-array type '" + at->name + "'", c->loc);
            return at;
        }
        std::vector<IntRange> ranges;
        bool open = false;
        for (std::size_t k = 0; k < c->args.size(); ++k) {
            const Expr& a = *c->args[k].actual;
            if (a.kind == ExprKind::Open) {
                open = true;
                if (k < at->indexRanges.size() && at->indexRanges[k])
                    ranges.push_back(*at->indexRanges[k]);
                else
                    ranges.push_back({0, -1, true});
                continue;
            }
            auto r = discreteRange(a, ctx);
            if (!r)
                return at;
            ranges.push_back(*r);
        }
        if (ranges.size() != at->indexTypes.size()) {
            error("VHDL-TYPE",
                  "array '" + at->name + "' has " + std::to_string(at->indexTypes.size()) + " dimension(s), constraint has " +
                      std::to_string(ranges.size()),
                  c->loc);
            return at;
        }
        const Type* sub = at;
        if (!open || std::ranges::any_of(ranges, [](const IntRange& r) { return !r.isNull(); }))
            sub = types.constrainedArray(at, ranges);
        if (level + 1 < constraintLists.size()) {
            Type copy = *sub;
            copy.element = constrain(sub->element, level + 1);
            copy.base = &sub->baseType();
            copy.isSubtype = true;
            return types.make(std::move(copy));
        }
        return sub;
    };
    t = constrain(t, 0);
    if (s.range) {
        if (s.range->kind == ExprKind::Box)
            return t;
        if (t->isRealLike()) {
            const Expr& r = *s.range;
            if (r.kind == ExprKind::Range) {
                auto l = constValue(*r.lhs, ctx, t);
                auto h = constValue(*r.rhs, ctx, t);
                if (l && h && l->kind == Value::Kind::Real && h->kind == Value::Kind::Real) {
                    Type copy = *t;
                    copy.base = &t->baseType();
                    copy.isSubtype = true;
                    copy.constrained = true;
                    copy.rrange = {l->r, h->r, r.dir == RangeDir::To};
                    return types.make(std::move(copy));
                }
            }
            return t;
        }
        auto r = discreteRange(*s.range, ctx);
        if (!r)
            return t;
        if (!t->isDiscrete() && t->kind != TypeKind::Physical) {
            error("VHDL-TYPE", "range constraint on non-scalar type '" + t->name + "'", s.loc);
            return t;
        }
        t = types.rangeSubtype(t, *r);
    }
    if (resolved && !t->resolved) {
        Type copy = *t;
        copy.base = &t->baseType();
        copy.isSubtype = true;
        copy.resolved = true;
        t = types.make(std::move(copy));
    }
    return t;
}

const Type* Elaborator::elaborateTypeDef(Scope& scope, const Ident& name, const TypeDef& def, Ctx& ctx) {
    Type t;
    t.name = name.text;
    switch (def.kind) {
        case TypeDef::Kind::Enum: {
            t.kind = TypeKind::Enum;
            auto info = std::make_shared<EnumInfo>();
            for (const auto& lit : def.literals) {
                info->keys.push_back(lit.key);
                info->display.push_back(lit.text);
            }
            t.enumInfo = info;
            t.constrained = true;
            t.range = {0, static_cast<std::int64_t>(info->keys.size()) - 1, true};
            const Type* tp = types.make(std::move(t));
            for (std::size_t k = 0; k < def.literals.size(); ++k) {
                Symbol* s = newSymbol(SymKind::EnumLiteral, def.literals[k].key, def.literals[k].text,
                                      def.literals[k].loc);
                s->type = tp;
                s->pos = static_cast<std::int64_t>(k);
                scope.add(s);
            }
            return tp;
        }
        case TypeDef::Kind::Range: {
            const Expr& r = *def.range;
            if (r.kind == ExprKind::Range) {
                TypeSet ls = typesOf(*r.lhs, ctx);
                if (ls.univReal || std::ranges::any_of(ls.types, [](const Type* x) { return x->isRealLike(); })) {
                    auto l = constValue(*r.lhs, ctx, std_.real);
                    auto h = constValue(*r.rhs, ctx, std_.real);
                    t.kind = TypeKind::Real;
                    t.constrained = true;
                    if (l && h)
                        t.rrange = {l->r, h->r, r.dir == RangeDir::To};
                    return types.make(std::move(t));
                }
            }
            auto rr = discreteRange(r, ctx);
            t.kind = TypeKind::Integer;
            t.constrained = true;
            t.range = rr.value_or(IntRange{0, 0, true});
            const Type* base = types.make(std::move(t));
            return base;
        }
        case TypeDef::Kind::Physical: {
            t.kind = TypeKind::Physical;
            t.constrained = true;
            if (auto rr = discreteRange(*def.range, ctx))
                t.range = *rr;
            auto units = std::make_shared<std::vector<PhysicalUnitInfo>>();
            t.units = units;
            const Type* tp = types.make(std::move(t));
            for (const auto& u : def.units) {
                std::int64_t mult = 1;
                if (u.value) {
                    // value is "n unit" (Physical expression) or a unit name
                    const Expr& v = *u.value;
                    std::int64_t n = 1;
                    std::string unitKey;
                    if (v.kind == ExprKind::Physical) {
                        n = v.lhs ? v.lhs->ival : 1;
                        unitKey = v.ident.key;
                    }
                    else if (v.kind == ExprKind::Simple) {
                        unitKey = v.ident.key;
                    }
                    for (const auto& prev : *units)
                        if (prev.key == unitKey)
                            mult = n * prev.multiplier;
                }
                units->push_back({u.name.key, mult});
                Symbol* s = newSymbol(SymKind::Unit, u.name.key, u.name.text, u.name.loc);
                s->type = tp;
                s->pos = mult;
                scope.add(s);
            }
            return tp;
        }
        case TypeDef::Kind::Array: {
            t.kind = TypeKind::Array;
            for (const auto& ix : def.indexes) {
                if (ix->kind == ExprKind::Subtype && ix->subtype->range && ix->subtype->range->kind == ExprKind::Box) {
                    const Type* it = typeMark(*ix->subtype->mark, ctx);
                    t.indexTypes.push_back(it ? it : std_.integer);
                    t.indexRanges.emplace_back(std::nullopt);
                    continue;
                }
                const Type* it = nullptr;
                auto r = discreteRange(*ix, ctx, &it);
                t.indexTypes.push_back(it ? it : std_.integer);
                if (r)
                    t.indexRanges.emplace_back(*r);
                else
                    t.indexRanges.emplace_back(IntRange{0, -1, true});
            }
            t.element = def.element ? subtype(*def.element, ctx) : nullptr;
            if (!t.element)
                t.element = std_.bit;
            return types.make(std::move(t));
        }
        case TypeDef::Kind::Record: {
            t.kind = TypeKind::Record;
            for (const auto& f : def.fields) {
                const Type* ft = subtype(*f.subtype, ctx);
                for (const auto& n : f.names)
                    t.fields.push_back({n.key, n.text, ft ? ft : std_.bit});
            }
            return types.make(std::move(t));
        }
        case TypeDef::Kind::Access: t.kind = TypeKind::Access; return types.make(std::move(t));
        case TypeDef::Kind::File: t.kind = TypeKind::File; return types.make(std::move(t));
        case TypeDef::Kind::Protected: t.kind = TypeKind::Protected; return types.make(std::move(t));
        case TypeDef::Kind::Incomplete: t.kind = TypeKind::Incomplete; return types.make(std::move(t));
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Declarations
// ---------------------------------------------------------------------------

void Elaborator::resolveParams(Symbol& sub) {
    if (sub.paramsResolved || !sub.decl)
        return;
    sub.paramsResolved = true;
    Ctx ctx;
    ctx.scope = sub.declScope;
    ctx.constOnly = true;
    for (const auto& p : sub.decl->params) {
        const Type* t = p.subtype ? subtype(*p.subtype, ctx) : nullptr;
        for (const auto& n : p.names) {
            ParamInfo pi;
            pi.key = n.key;
            pi.display = n.text;
            pi.cls = p.cls;
            pi.mode = p.mode == Mode::None ? Mode::In : p.mode;
            if (pi.cls == InterfaceDecl::Class::Default)
                pi.cls = (pi.mode == Mode::In && sub.isFunction) ? InterfaceDecl::Class::Constant
                         : pi.mode == Mode::In                   ? InterfaceDecl::Class::Constant
                                                                 : InterfaceDecl::Class::Variable;
            pi.type = t;
            pi.defaultExpr = p.init.get();
            pi.defaultScope = sub.declScope;
            sub.params.push_back(std::move(pi));
        }
    }
    if (sub.isFunction && sub.decl->returnType)
        sub.type = typeMark(*sub.decl->returnType, ctx);
}

namespace {

/// Same parameter profile (by type-mark spelling): binds a subprogram body
/// to its declaration.
bool sameProfile(const Subprogram& a, const Subprogram& b) {
    auto flat = [](const Subprogram& s) {
        std::vector<std::string> v;
        for (const auto& p : s.params) {
            std::string mark;
            const Expr* m = p.subtype ? p.subtype->mark.get() : nullptr;
            while (m && m->kind == ExprKind::Call)
                m = m->prefix.get();
            if (m)
                mark = m->ident.key;
            for (std::size_t k = 0; k < p.names.size(); ++k)
                v.push_back(mark);
        }
        if (s.returnType)
            v.push_back("return " + (s.returnType->kind == ExprKind::Simple || s.returnType->kind == ExprKind::Selected
                                         ? s.returnType->ident.key
                                         : std::string()));
        return v;
    };
    return a.isFunction == b.isFunction && flat(a) == flat(b);
}

} // namespace

void Elaborator::bindSubprogramBody(Scope& scope, Symbol& def) {
    // A body completes a declaration made earlier in this scope or, for a
    // package body, in the package declaration.
    auto tryScope = [&](Scope& s) {
        const auto* v = s.local(def.key);
        if (!v)
            return false;
        for (Symbol* cand : *v) {
            if (cand == &def || cand->kind != SymKind::Subprogram || !cand->decl || cand->body)
                continue;
            if (sameProfile(*cand->decl, *def.decl)) {
                cand->body = def.decl;
                cand->bodyScope = &scope;
                def.body = def.decl;
                def.bodyScope = &scope;
                def.implicitOp = true; // hidden duplicate
                return true;
            }
        }
        return false;
    };
    if (tryScope(scope))
        return;
    if (scope.parent && scope.label.ends_with(" body") && tryScope(*scope.parent))
        return;
    def.body = def.decl;
    def.bodyScope = &scope;
}

void Elaborator::declareInterface(Scope& scope, const InterfaceDecl& d, Ctx& ctx) {
    // generics of entities/packages/blocks: constants
    if (d.cls == InterfaceDecl::Class::Type) {
        for (const auto& n : d.names) {
            Symbol* s = newSymbol(SymKind::Type, n.key, n.text, n.loc);
            s->type = std_.integer; // unbound generic type: placeholder
            scope.add(s);
        }
        return;
    }
    if (d.cls == InterfaceDecl::Class::Function || d.cls == InterfaceDecl::Class::Procedure ||
        d.cls == InterfaceDecl::Class::Package) {
        unsupported("IR-UNSUPPORTED-GENERIC", "subprogram/package generic '" + d.names.front().text + "'", d.loc);
        return;
    }
    const Type* t = d.subtype ? subtype(*d.subtype, ctx) : nullptr;
    for (const auto& n : d.names) {
        Symbol* s = newSymbol(SymKind::Constant, n.key, n.text, n.loc);
        s->type = t;
        if (d.init) {
            s->value = eval(*d.init, ctx, t);
            if (s->value.valid() && t && t->isArray() && !t->isFullyConstrained())
                s->type = s->value.type;
        }
        scope.add(s);
    }
}

void Elaborator::declare(Scope& scope, const Decl& d, Ctx& ctx) {
    switch (d.kind) {
        case DeclKind::Constant: {
            const Type* t = d.subtype ? subtype(*d.subtype, ctx) : nullptr;
            for (const auto& n : d.names) {
                // deferred constant completed in a package body
                if (d.init && scope.label.ends_with(" body") && scope.parent) {
                    if (const auto* v = scope.parent->local(n.key)) {
                        Symbol* prev = v->front();
                        if (prev->kind == SymKind::Constant && prev->deferred) {
                            Ctx c2 = ctx;
                            prev->value = eval(*d.init, c2, t);
                            if (prev->value.valid())
                                prev->type = (t && t->isFullyConstrained()) ? t : prev->value.type;
                            prev->deferred = false;
                            continue;
                        }
                    }
                }
                Symbol* s = newSymbol(SymKind::Constant, n.key, n.text, n.loc);
                s->type = t;
                if (d.init) {
                    s->value = eval(*d.init, ctx, t);
                    if (s->value.valid()) {
                        if (!t || (t->isArray() && !t->isFullyConstrained()))
                            s->type = s->value.type;
                        else if (s->value.type != t)
                            s->value = convert(s->value, t, ctx, d.loc);
                    }
                }
                else {
                    s->deferred = true;
                }
                scope.add(s);
            }
            return;
        }
        case DeclKind::Signal:
        case DeclKind::Variable:
        case DeclKind::SharedVariable: {
            const Type* t = d.subtype ? subtype(*d.subtype, ctx) : nullptr;
            if (!t)
                return;
            if (d.kind == DeclKind::Variable && ctx.inlineDepth > 0) {
                // subprogram local (also during constant evaluation outside any body): no storage
                SVal init;
                if (d.init) {
                    init = eval(*d.init, ctx, t);
                    if (init.valid() && t->isArray() && !t->isFullyConstrained())
                        t = init.type;
                }
                for (const auto& n : d.names) {
                    Symbol* s = newSymbol(SymKind::Variable, n.key, n.text, n.loc);
                    s->type = t;
                    Object o;
                    o.name = n.text;
                    o.type = t;
                    o.loc = n.loc;
                    o.initial = init.valid() ? convert(init, t, ctx, n.loc) : initialValue(o, n.loc);
                    s->object = addObject(std::move(o));
                    scope.add(s);
                }
                return;
            }
            if (!body) {
                // package-level signals and shared variables
                for (const auto& n : d.names) {
                    Symbol* s = newSymbol(d.kind == DeclKind::Signal ? SymKind::Signal : SymKind::Variable, n.key,
                                          n.text, n.loc);
                    s->type = t;
                    scope.add(s);
                }
                if (d.kind == DeclKind::Signal)
                    unsupported("IR-UNSUPPORTED-GLOBAL-SIGNAL",
                                "signal '" + d.names.front().text + "' declared in a package", d.loc);
                return;
            }
            SVal init;
            if (d.init) {
                init = eval(*d.init, ctx, t);
                if (init.valid() && t->isArray() && !t->isFullyConstrained())
                    t = init.type;
            }
            if (!t->isFullyConstrained()) {
                error("VHDL-UNCONSTRAINED", "object '" + d.names.front().text + "' has an unconstrained type", d.loc);
                return;
            }
            for (const auto& n : d.names) {
                Symbol* s = newSymbol(d.kind == DeclKind::Signal ? SymKind::Signal : SymKind::Variable, n.key, n.text,
                                      n.loc);
                s->type = t;
                // process variables are named "<process label>.<variable>"
                std::string name = body->prefix + (ctx.st && d.kind == DeclKind::Variable && !scope.label.empty()
                                                       ? scope.label + "."
                                                       : std::string()) +
                                   n.text;
                s->object = createStorageObject(name, t, d.kind == DeclKind::Signal, Mode::None, n.loc, {},
                                                init.valid() ? &init : nullptr);
                scope.add(s);
            }
            return;
        }
        case DeclKind::File:
            for (const auto& n : d.names) {
                Symbol* s = newSymbol(SymKind::File, n.key, n.text, n.loc);
                s->type = d.subtype ? subtype(*d.subtype, ctx) : nullptr;
                scope.add(s);
            }
            return;
        case DeclKind::Type: {
            const Ident& n = d.names.front();
            // completion of an incomplete type declaration
            const Type* t = elaborateTypeDef(scope, n, *d.type, ctx);
            Symbol* s = newSymbol(SymKind::Type, n.key, n.text, n.loc);
            s->type = t;
            scope.add(s);
            return;
        }
        case DeclKind::Subtype: {
            const Ident& n = d.names.front();
            const Type* t = d.subtype ? subtype(*d.subtype, ctx) : nullptr;
            if (!t)
                return;
            if (t->name != n.text) {
                Type copy = *t;
                copy.name = n.text;
                copy.base = &t->baseType();
                copy.isSubtype = true;
                t = types.make(std::move(copy));
            }
            Symbol* s = newSymbol(SymKind::Type, n.key, n.text, n.loc);
            s->type = t;
            scope.add(s);
            return;
        }
        case DeclKind::Alias: {
            const Ident& n = d.names.front();
            // alias of a non-object (type, subprogram, enum literal): re-export the symbols
            auto targets = d.init->kind == ExprKind::Simple || d.init->kind == ExprKind::Selected
                               ? resolveName(*d.init, ctx, true)
                               : std::vector<Symbol*>{};
            if (!targets.empty() && (targets.front()->kind == SymKind::Type ||
                                     targets.front()->kind == SymKind::Subprogram ||
                                     targets.front()->kind == SymKind::EnumLiteral)) {
                for (Symbol* t : targets) {
                    Symbol* s = newSymbol(t->kind, n.key, n.text, n.loc);
                    *s = *t;
                    s->key = n.key;
                    s->display = n.text;
                    scope.add(s);
                }
                return;
            }
            Symbol* s = newSymbol(SymKind::Alias, n.key, n.text, n.loc);
            s->aliasTarget = d.init.get();
            s->aliasScope = &scope;
            s->aliasSubtype = d.subtype.get();
            TypeSet ts = typesOf(*d.init, ctx);
            s->type = d.subtype ? subtype(*d.subtype, ctx) : (ts.types.size() == 1 ? ts.types.front() : nullptr);
            scope.add(s);
            return;
        }
        case DeclKind::Component: {
            Symbol* s = newSymbol(SymKind::Component, d.names.front().key, d.names.front().text, d.loc);
            s->component = &d;
            scope.add(s);
            return;
        }
        case DeclKind::AttributeDecl: {
            Symbol* s = newSymbol(SymKind::AttributeDecl, d.names.front().key, d.names.front().text, d.loc);
            s->type = d.subtype ? subtype(*d.subtype, ctx) : std_.string;
            scope.add(s);
            return;
        }
        case DeclKind::AttributeSpec: {
            const std::string& attr = d.names.front().key;
            const Type* at = std_.string;
            for (Symbol* a : lookup(scope, attr))
                if (a->kind == SymKind::AttributeDecl && a->type)
                    at = a->type;
            SVal v = eval(*d.init, ctx, at);
            auto apply = [&](Symbol* target) {
                target->attributes.emplace_back(attr, v);
                if ((target->kind == SymKind::Signal || target->kind == SymKind::Variable) && body &&
                    target->object >= 0 && target->object < kFrameBase) {
                    // attributes on signals become net attributes (ASYNC_REG, ...)
                    // same rendering as Verilog attributes: strings keep their quotes
                    std::string text = v.known() ? formatValue(*v.type, *v.value) : std::string();
                    for (const auto& ch : object(target->object).storage.chunks())
                        if (!ch.isConst())
                            body->body.net(ch.net).attrs.push_back({d.names.front().text, text});
                }
            };
            if (d.entityClass == "label") {
                // attributes of statement labels (instances): ASYNC_REG on a primitive
                std::string text = v.known() ? formatValue(*v.type, *v.value) : std::string();
                for (const auto& en : d.entityNames)
                    scope.labelAttrs[en.key].push_back({d.names.front().text, text});
                return;
            }
            for (const auto& en : d.entityNames) {
                if (en.key == "all" || en.key == "others") {
                    for (const auto& k : scope.order)
                        for (Symbol* sym : scope.names[k])
                            if (sym->kind == SymKind::Signal)
                                apply(sym);
                    continue;
                }
                if (const auto* v2 = scope.local(en.key)) {
                    for (Symbol* sym : *v2)
                        apply(sym);
                }
                // labels and entity names: attribute recorded nowhere (no symbol); fine
            }
            return;
        }
        case DeclKind::Subprogram: {
            const Subprogram& sp = *d.sub;
            Symbol* s = newSymbol(SymKind::Subprogram, sp.designator.key, sp.designator.text, d.loc);
            s->isFunction = sp.isFunction;
            s->pure = sp.pure;
            s->decl = &sp;
            s->declScope = &scope;
            if (sp.hasBody) {
                bindSubprogramBody(scope, *s);
                if (s->implicitOp)
                    return; // completes an existing declaration
            }
            scope.add(s);
            return;
        }
        case DeclKind::Use: {
            ContextItem item;
            item.kind = ContextItem::Kind::Use;
            item.loc = d.loc;
            // applyContext only reads the names: reuse the Decl's expressions
            for (const auto& n : d.useNames) {
                Ctx c2;
                c2.scope = &scope;
                c2.constOnly = true;
                const Expr& name = *n;
                if (name.kind != ExprKind::Selected)
                    continue;
                if (name.ident.key == "all") {
                    auto syms = resolveName(*name.prefix, c2);
                    if (!syms.empty() && syms.front()->kind == SymKind::Package)
                        scope.usedAll.push_back(&syms.front()->package->scope);
                }
                else {
                    auto pre = resolveName(*name.prefix, c2);
                    if (!pre.empty() && pre.front()->kind == SymKind::Package)
                        scope.usedItems.emplace_back(&pre.front()->package->scope, name.ident.key);
                }
            }
            return;
        }
        case DeclKind::PackageInst: {
            // package p is new lib.gp generic map (...): handled like a design unit
            auto p = std::make_unique<PackageInfo>();
            p->library = scope.workLibrary;
            p->name = d.names.front().key;
            auto unit = std::make_unique<DesignUnit>();
            unit->kind = UnitKind::PackageInstance;
            unit->name = d.names.front();
            unit->loc = d.loc;
            // non-owning: the Decl outlives the elaboration
            unit->instantiated.reset(const_cast<Expr*>(d.unit.get()));
            for (const auto& a : d.genericMap) {
                Assoc copy;
                copy.loc = a.loc;
                copy.formal.reset(a.formal.get());
                copy.actual.reset(a.actual.get());
                unit->genericMap.push_back(std::move(copy));
            }
            p->decl = unit.get();
            elaboratePackage(*p);
            // release non-owning pointers
            (void)unit->instantiated.release();
            for (auto& a : unit->genericMap) {
                (void)a.formal.release();
                (void)a.actual.release();
            }
            p->decl = nullptr;
            Symbol* s = newSymbol(SymKind::Package, d.names.front().key, d.names.front().text, d.loc);
            s->package = p.get();
            scope.add(s);
            packages.emplace(std::make_pair(scope.workLibrary + "#local", p->name + std::to_string(symbols.size())),
                             std::move(p));
            return;
        }
        case DeclKind::Ignored: return;
    }
}

} // namespace opencdc::vhdl
