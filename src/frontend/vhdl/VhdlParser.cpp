#include "frontend/vhdl/VhdlParser.h"

#include "frontend/vhdl/VhdlLexer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <utility>

namespace opencdc::vhdl {

const char* opSymbol(Op op) noexcept {
    switch (op) {
        case Op::None: return "";
        case Op::And: return "and";
        case Op::Or: return "or";
        case Op::Nand: return "nand";
        case Op::Nor: return "nor";
        case Op::Xor: return "xor";
        case Op::Xnor: return "xnor";
        case Op::Eq: return "=";
        case Op::Ne: return "/=";
        case Op::Lt: return "<";
        case Op::Le: return "<=";
        case Op::Gt: return ">";
        case Op::Ge: return ">=";
        case Op::MatchEq: return "?=";
        case Op::MatchNe: return "?/=";
        case Op::MatchLt: return "?<";
        case Op::MatchLe: return "?<=";
        case Op::MatchGt: return "?>";
        case Op::MatchGe: return "?>=";
        case Op::Sll: return "sll";
        case Op::Srl: return "srl";
        case Op::Sla: return "sla";
        case Op::Sra: return "sra";
        case Op::Rol: return "rol";
        case Op::Ror: return "ror";
        case Op::Add: return "+";
        case Op::Sub: return "-";
        case Op::Concat: return "&";
        case Op::Mul: return "*";
        case Op::Div: return "/";
        case Op::Mod: return "mod";
        case Op::Rem: return "rem";
        case Op::Pow: return "**";
        case Op::Abs: return "abs";
        case Op::Not: return "not";
        case Op::Plus: return "+";
        case Op::Minus: return "-";
        case Op::Condition: return "??";
        case Op::RedAnd: return "and";
        case Op::RedOr: return "or";
        case Op::RedNand: return "nand";
        case Op::RedNor: return "nor";
        case Op::RedXor: return "xor";
        case Op::RedXnor: return "xnor";
    }
    return "";
}

const char* toString(UnitKind k) noexcept {
    switch (k) {
        case UnitKind::Entity: return "entity";
        case UnitKind::Architecture: return "architecture";
        case UnitKind::Package: return "package";
        case UnitKind::PackageBody: return "package body";
        case UnitKind::PackageInstance: return "package instance";
        case UnitKind::Configuration: return "configuration";
        case UnitKind::Context: return "context";
    }
    return "";
}

bool ParseResult::hasErrors() const noexcept {
    return std::ranges::any_of(messages, [](const ParseMessage& m) { return m.error; });
}

namespace {

struct ParseError {
    std::string message;
    Loc loc;
};

/// Value of an abstract literal (decimal or based, integer or real).
void literalValue(Expr& e, std::string_view text) {
    std::string t;
    for (const char c : text)
        if (c != '_')
            t += c;
    const auto hash = t.find_first_of("#:");
    if (hash != std::string::npos) {
        const int base = std::atoi(t.substr(0, hash).c_str());
        const auto end = t.find(t[hash], hash + 1);
        const std::string digits = t.substr(hash + 1, end - hash - 1);
        long exp = 0;
        if (end != std::string::npos && end + 1 < t.size() && (t[end + 1] == 'e' || t[end + 1] == 'E'))
            exp = std::strtol(t.c_str() + end + 2, nullptr, 10);
        const bool real = digits.find('.') != std::string::npos;
        e.kind = real ? ExprKind::Real : ExprKind::Integer;
        long double v = 0;
        long double frac = 0;
        long double scale = 1;
        bool afterPoint = false;
        std::uint64_t iv = 0;
        for (const char c : digits) {
            if (c == '.') {
                afterPoint = true;
                continue;
            }
            const int d = std::isdigit(static_cast<unsigned char>(c)) != 0 ? c - '0' : (std::tolower(c) - 'a' + 10);
            if (afterPoint) {
                scale /= base;
                frac += d * scale;
            }
            else {
                v = v * base + d;
                const std::uint64_t next = iv * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(d);
                if (next / static_cast<std::uint64_t>(base) != iv)
                    e.overflow = true;
                iv = next;
            }
        }
        const long double total = (v + frac) * std::pow(static_cast<long double>(base), static_cast<long double>(exp));
        e.rval = static_cast<double>(total);
        if (!real) {
            for (long k = 0; k < exp; ++k) {
                const std::uint64_t next = iv * static_cast<std::uint64_t>(base);
                if (next / static_cast<std::uint64_t>(base) != iv)
                    e.overflow = true;
                iv = next;
            }
            if (iv > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                e.overflow = true;
            e.ival = static_cast<std::int64_t>(iv);
        }
        return;
    }
    const bool real = t.find('.') != std::string::npos;
    e.kind = real ? ExprKind::Real : ExprKind::Integer;
    e.rval = std::strtod(t.c_str(), nullptr);
    if (!real) {
        const auto epos = t.find_first_of("eE");
        const std::string mant = t.substr(0, epos);
        const long exp = epos == std::string::npos ? 0 : std::strtol(t.c_str() + epos + 1, nullptr, 10);
        std::uint64_t iv = 0;
        for (const char c : mant) {
            const std::uint64_t next = iv * 10 + static_cast<std::uint64_t>(c - '0');
            if (next / 10 != iv)
                e.overflow = true;
            iv = next;
        }
        for (long k = 0; k < exp; ++k) {
            const std::uint64_t next = iv * 10;
            if (next / 10 != iv)
                e.overflow = true;
            iv = next;
        }
        if (iv > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            e.overflow = true;
        e.ival = static_cast<std::int64_t>(iv);
    }
}

class Parser {
public:
    Parser(std::vector<Token> toks, std::vector<ParseMessage>& msgs) : t_(std::move(toks)), msgs_(msgs) {}

    DesignFile designFile() {
        DesignFile f;
        while (!at(Tok::End)) {
            const std::size_t start = p_;
            try {
                f.units.push_back(designUnit());
            }
            catch (const ParseError& e) {
                error(e);
                recoverToUnit(start);
            }
        }
        return f;
    }

    ExprPtr singleExpression() {
        try {
            auto e = expression();
            if (!at(Tok::End))
                throw err("unexpected text after expression");
            return e;
        }
        catch (const ParseError& e) {
            error(e);
            return nullptr;
        }
    }

private:
    // ------------------------------------------------------------------
    // token helpers
    // ------------------------------------------------------------------
    [[nodiscard]] const Token& tok(std::size_t k = 0) const {
        const std::size_t i = std::min(p_ + k, t_.size() - 1);
        return t_[i];
    }
    [[nodiscard]] bool at(Tok k, std::size_t ahead = 0) const { return tok(ahead).kind == k; }
    [[nodiscard]] bool atKw(Kw k, std::size_t ahead = 0) const {
        return tok(ahead).kind == Tok::Keyword && tok(ahead).kw == k;
    }
    /// Identifier with this lower-case spelling (non-reserved words such as
    /// "context", "force", "default").
    [[nodiscard]] bool atWord(std::string_view w, std::size_t ahead = 0) const {
        return tok(ahead).kind == Tok::Identifier && tok(ahead).value == w;
    }
    [[nodiscard]] bool atIdent(std::size_t ahead = 0) const {
        return at(Tok::Identifier, ahead) || at(Tok::ExtIdentifier, ahead);
    }
    [[nodiscard]] Loc loc() const { return tok().loc; }
    const Token& advance() {
        const Token& t = tok();
        if (p_ + 1 < t_.size())
            ++p_;
        return t;
    }
    bool accept(Tok k) {
        if (!at(k))
            return false;
        advance();
        return true;
    }
    bool acceptKw(Kw k) {
        if (!atKw(k))
            return false;
        advance();
        return true;
    }
    [[nodiscard]] ParseError err(std::string msg) const { return {std::move(msg), loc()}; }

    static std::string describe(const Token& t) {
        switch (t.kind) {
            case Tok::End: return "end of file";
            case Tok::Keyword: return "'" + std::string(keywordText(t.kw)) + "'";
            case Tok::StringLiteral: return "string \"" + t.text + "\"";
            case Tok::BitStringLiteral: return "bit string " + t.value;
            default: break;
        }
        if (!t.text.empty())
            return "'" + t.text + "'";
        return "'" + std::string(tokText(t.kind)) + "'";
    }
    static const char* tokText(Tok k) {
        switch (k) {
            case Tok::Amp: return "&";
            case Tok::Tick: return "'";
            case Tok::LParen: return "(";
            case Tok::RParen: return ")";
            case Tok::Star: return "*";
            case Tok::Plus: return "+";
            case Tok::Comma: return ",";
            case Tok::Minus: return "-";
            case Tok::Dot: return ".";
            case Tok::Slash: return "/";
            case Tok::Colon: return ":";
            case Tok::Semicolon: return ";";
            case Tok::Lt: return "<";
            case Tok::Eq: return "=";
            case Tok::Gt: return ">";
            case Tok::Bar: return "|";
            case Tok::LBracket: return "[";
            case Tok::RBracket: return "]";
            case Tok::Arrow: return "=>";
            case Tok::DoubleStar: return "**";
            case Tok::VarAssign: return ":=";
            case Tok::Ne: return "/=";
            case Tok::Ge: return ">=";
            case Tok::Le: return "<=";
            case Tok::Box: return "<>";
            case Tok::Cond: return "??";
            case Tok::DLt: return "<<";
            case Tok::DGt: return ">>";
            case Tok::Identifier: return "identifier";
            default: return "token";
        }
    }

    void expect(Tok k) {
        if (!accept(k))
            throw err(std::string("expected '") + tokText(k) + "' but found " + describe(tok()));
    }
    void expectKw(Kw k) {
        if (!acceptKw(k))
            throw err(std::string("expected '") + keywordText(k) + "' but found " + describe(tok()));
    }

    static Ident identOf(const Token& t) {
        Ident id;
        id.text = t.text;
        id.loc = t.loc;
        switch (t.kind) {
            case Tok::Identifier:
            case Tok::Keyword: id.key = lowerAscii(t.text); break;
            case Tok::ExtIdentifier: id.key = t.text; break;
            case Tok::CharLiteral: id.key = t.text; break;
            case Tok::StringLiteral:
                id.text = "\"" + t.text + "\"";
                id.key = "\"" + lowerAscii(t.text) + "\"";
                break;
            default: id.key = t.text; break;
        }
        return id;
    }

    Ident identifier() {
        if (!atIdent())
            throw err("expected identifier but found " + describe(tok()));
        return identOf(advance());
    }

    void error(const ParseError& e) { msgs_.push_back({e.message, e.loc, true}); }
    void warning(std::string msg, Loc l) { msgs_.push_back({std::move(msg), l, false}); }

    /// Skips to just after the next ';' at parenthesis depth 0.
    void skipToSemicolon() {
        int depth = 0;
        while (!at(Tok::End)) {
            if (at(Tok::LParen))
                ++depth;
            else if (at(Tok::RParen) && depth > 0)
                --depth;
            else if (at(Tok::Semicolon) && depth == 0) {
                advance();
                return;
            }
            advance();
        }
    }

    [[nodiscard]] bool atUnitStart() const {
        if (atKw(Kw::Library))
            return true;
        if (atKw(Kw::Entity) && atIdent(1) && atKw(Kw::Is, 2))
            return true;
        if (atKw(Kw::Architecture) && atIdent(1) && atKw(Kw::Of, 2))
            return true;
        if (atKw(Kw::Package) && (atKw(Kw::Body, 1) || (atIdent(1) && atKw(Kw::Is, 2))))
            return true;
        if (atKw(Kw::Configuration) && atIdent(1) && atKw(Kw::Of, 2))
            return true;
        return atWord("context") && atIdent(1) && atKw(Kw::Is, 2);
    }

    void recoverToUnit(std::size_t start) {
        if (p_ == start)
            advance();
        while (!at(Tok::End) && !atUnitStart())
            advance();
    }

    /// Optional closing words after "end": keywords then an identifier.
    void endTail(std::initializer_list<Kw> kws) {
        for (const Kw k : kws)
            if (!acceptKw(k))
                break;
        if (atIdent() || at(Tok::StringLiteral) || at(Tok::CharLiteral))
            advance();
        expect(Tok::Semicolon);
    }

    // ------------------------------------------------------------------
    // design units
    // ------------------------------------------------------------------
    std::vector<ContextItem> contextClause() {
        std::vector<ContextItem> items;
        for (;;) {
            if (atKw(Kw::Library)) {
                ContextItem c;
                c.kind = ContextItem::Kind::Library;
                c.loc = loc();
                advance();
                c.libraries.push_back(identifier());
                while (accept(Tok::Comma))
                    c.libraries.push_back(identifier());
                expect(Tok::Semicolon);
                items.push_back(std::move(c));
            }
            else if (atKw(Kw::Use)) {
                ContextItem c;
                c.kind = ContextItem::Kind::Use;
                c.loc = loc();
                advance();
                c.names.push_back(name());
                while (accept(Tok::Comma))
                    c.names.push_back(name());
                expect(Tok::Semicolon);
                items.push_back(std::move(c));
            }
            else if (atWord("context") && !(atIdent(1) && atKw(Kw::Is, 2))) {
                ContextItem c;
                c.kind = ContextItem::Kind::Context;
                c.loc = loc();
                advance();
                c.names.push_back(name());
                while (accept(Tok::Comma))
                    c.names.push_back(name());
                expect(Tok::Semicolon);
                items.push_back(std::move(c));
            }
            else {
                return items;
            }
        }
    }

    DesignUnit designUnit() {
        DesignUnit u;
        u.context = contextClause();
        u.loc = loc();
        if (acceptKw(Kw::Entity)) {
            u.kind = UnitKind::Entity;
            u.name = identifier();
            expectKw(Kw::Is);
            if (atKw(Kw::Generic))
                u.generics = genericClause();
            if (atKw(Kw::Port))
                u.ports = portClause();
            u.decls = declarations();
            if (acceptKw(Kw::Begin))
                u.stmts = concurrentStatements();
            expectKw(Kw::End);
            endTail({Kw::Entity});
        }
        else if (acceptKw(Kw::Architecture)) {
            u.kind = UnitKind::Architecture;
            u.name = identifier();
            expectKw(Kw::Of);
            u.entity = identifier();
            expectKw(Kw::Is);
            u.decls = declarations();
            expectKw(Kw::Begin);
            u.stmts = concurrentStatements();
            expectKw(Kw::End);
            endTail({Kw::Architecture});
        }
        else if (acceptKw(Kw::Package)) {
            if (acceptKw(Kw::Body)) {
                u.kind = UnitKind::PackageBody;
                u.name = identifier();
                expectKw(Kw::Is);
                u.decls = declarations();
                expectKw(Kw::End);
                endTail({Kw::Package, Kw::Body});
            }
            else {
                u.name = identifier();
                expectKw(Kw::Is);
                if (acceptKw(Kw::New)) {
                    u.kind = UnitKind::PackageInstance;
                    u.instantiated = name();
                    if (acceptKw(Kw::Generic)) {
                        expectKw(Kw::Map);
                        u.genericMap = associationList();
                    }
                    expect(Tok::Semicolon);
                }
                else {
                    u.kind = UnitKind::Package;
                    if (atKw(Kw::Generic)) {
                        u.generics = genericClause();
                        if (acceptKw(Kw::Generic)) {
                            expectKw(Kw::Map);
                            u.genericMap = associationList();
                            expect(Tok::Semicolon);
                        }
                    }
                    u.decls = declarations();
                    expectKw(Kw::End);
                    endTail({Kw::Package});
                }
            }
        }
        else if (acceptKw(Kw::Configuration)) {
            u.kind = UnitKind::Configuration;
            u.name = identifier();
            expectKw(Kw::Of);
            u.entity = identifier();
            expectKw(Kw::Is);
            skipConfigurationBody();
        }
        else if (atWord("context")) {
            advance();
            u.kind = UnitKind::Context;
            u.name = identifier();
            expectKw(Kw::Is);
            u.context = contextClause();
            expectKw(Kw::End);
            if (atWord("context"))
                advance();
            endTail({});
        }
        else {
            throw err("expected a design unit (entity, architecture, package, configuration) but found " +
                      describe(tok()));
        }
        return u;
    }

    /// Configuration declarations select architectures; synthesis of the
    /// default binding does not need them. Their body is skipped.
    void skipConfigurationBody() {
        int depth = 0;
        while (!at(Tok::End)) {
            if (atKw(Kw::For)) {
                ++depth;
                advance();
            }
            else if (atKw(Kw::End)) {
                if (atKw(Kw::For, 1)) {
                    --depth;
                    advance();
                    advance();
                    expect(Tok::Semicolon);
                }
                else if (depth <= 0) {
                    advance();
                    endTail({Kw::Configuration});
                    return;
                }
                else {
                    advance();
                }
            }
            else {
                advance();
            }
        }
        throw err("unterminated configuration declaration");
    }

    // ------------------------------------------------------------------
    // interface lists
    // ------------------------------------------------------------------
    std::vector<InterfaceDecl> genericClause() {
        expectKw(Kw::Generic);
        auto list = interfaceList(InterfaceDecl::Class::Constant);
        expect(Tok::Semicolon);
        return list;
    }
    std::vector<InterfaceDecl> portClause() {
        expectKw(Kw::Port);
        auto list = interfaceList(InterfaceDecl::Class::Signal);
        expect(Tok::Semicolon);
        return list;
    }

    std::vector<InterfaceDecl> interfaceList(InterfaceDecl::Class defaultClass) {
        expect(Tok::LParen);
        std::vector<InterfaceDecl> out;
        do {
            if (at(Tok::RParen))
                break; // tolerate a trailing ';' before ')' (common typo, accepted by tools)
            out.push_back(interfaceDecl(defaultClass));
        } while (accept(Tok::Semicolon));
        expect(Tok::RParen);
        return out;
    }

    InterfaceDecl interfaceDecl(InterfaceDecl::Class defaultClass) {
        InterfaceDecl d;
        d.loc = loc();
        d.cls = defaultClass;
        if (acceptKw(Kw::Type)) {
            d.cls = InterfaceDecl::Class::Type;
            d.names.push_back(identifier());
            return d;
        }
        if (atKw(Kw::Function) || atKw(Kw::Procedure) || atKw(Kw::Pure) || atKw(Kw::Impure)) {
            d.cls = atKw(Kw::Procedure) ? InterfaceDecl::Class::Procedure : InterfaceDecl::Class::Function;
            auto sub = subprogramSpec();
            d.names.push_back(sub->designator);
            if (acceptKw(Kw::Is)) {
                if (!accept(Tok::Box))
                    d.init = name();
            }
            return d;
        }
        if (acceptKw(Kw::Package)) {
            d.cls = InterfaceDecl::Class::Package;
            d.names.push_back(identifier());
            expectKw(Kw::Is);
            expectKw(Kw::New);
            d.init = name();
            expectKw(Kw::Generic);
            expectKw(Kw::Map);
            expect(Tok::LParen);
            int depth = 1;
            while (depth > 0 && !at(Tok::End)) {
                if (at(Tok::LParen))
                    ++depth;
                else if (at(Tok::RParen))
                    --depth;
                advance();
            }
            return d;
        }
        if (acceptKw(Kw::Constant))
            d.cls = InterfaceDecl::Class::Constant;
        else if (acceptKw(Kw::Signal))
            d.cls = InterfaceDecl::Class::Signal;
        else if (acceptKw(Kw::Variable))
            d.cls = InterfaceDecl::Class::Variable;
        else if (acceptKw(Kw::File))
            d.cls = InterfaceDecl::Class::File;
        d.names.push_back(identifier());
        while (accept(Tok::Comma))
            d.names.push_back(identifier());
        expect(Tok::Colon);
        if (acceptKw(Kw::In))
            d.mode = Mode::In;
        else if (acceptKw(Kw::Out))
            d.mode = Mode::Out;
        else if (acceptKw(Kw::Inout))
            d.mode = Mode::InOut;
        else if (acceptKw(Kw::Buffer))
            d.mode = Mode::Buffer;
        else if (acceptKw(Kw::Linkage))
            d.mode = Mode::Linkage;
        d.subtype = subtypeIndication();
        if (acceptKw(Kw::Bus))
            d.bus = true;
        if (accept(Tok::VarAssign))
            d.init = expression();
        return d;
    }

    // ------------------------------------------------------------------
    // declarations
    // ------------------------------------------------------------------
    [[nodiscard]] bool atDeclarationStart() const {
        if (tok().kind == Tok::Keyword) {
            switch (tok().kw) {
                case Kw::Signal:
                case Kw::Variable:
                case Kw::Shared:
                case Kw::Constant:
                case Kw::File:
                case Kw::Type:
                case Kw::Subtype:
                case Kw::Alias:
                case Kw::Component:
                case Kw::Attribute:
                case Kw::Function:
                case Kw::Procedure:
                case Kw::Pure:
                case Kw::Impure:
                case Kw::Use:
                case Kw::Group:
                case Kw::Disconnect:
                case Kw::Package:
                case Kw::For: return true;
                default: return false;
            }
        }
        // PSL declarations inside architectures
        return (atWord("default") && atWord("clock", 1)) || atWord("property") || atWord("sequence");
    }

    std::vector<DeclPtr> declarations() {
        std::vector<DeclPtr> out;
        while (atDeclarationStart()) {
            const std::size_t start = p_;
            try {
                out.push_back(declaration());
            }
            catch (const ParseError& e) {
                error(e);
                p_ = std::max(p_, start);
                if (p_ == start)
                    advance();
                skipToSemicolon();
            }
        }
        return out;
    }

    DeclPtr declaration() {
        auto d = std::make_unique<Decl>();
        d->loc = loc();
        if (atWord("default") || atWord("property") || atWord("sequence")) {
            d->kind = DeclKind::Ignored;
            d->what = "PSL declaration";
            skipToSemicolon();
            return d;
        }
        switch (tok().kw) {
            case Kw::Signal:
            case Kw::Variable:
            case Kw::Constant:
            case Kw::File:
            case Kw::Shared: {
                if (acceptKw(Kw::Shared)) {
                    expectKw(Kw::Variable);
                    d->kind = DeclKind::SharedVariable;
                }
                else {
                    const Kw k = advance().kw;
                    d->kind = k == Kw::Signal     ? DeclKind::Signal
                              : k == Kw::Variable ? DeclKind::Variable
                              : k == Kw::Constant ? DeclKind::Constant
                                                  : DeclKind::File;
                }
                d->names.push_back(identifier());
                while (accept(Tok::Comma))
                    d->names.push_back(identifier());
                expect(Tok::Colon);
                d->subtype = subtypeIndication();
                if (d->kind == DeclKind::File) {
                    // [open mode] is "name"
                    if (acceptKw(Kw::Open))
                        expression();
                    if (acceptKw(Kw::Is)) {
                        if (atKw(Kw::In) || atKw(Kw::Out))
                            advance();
                        d->init = expression();
                    }
                }
                else {
                    if (acceptKw(Kw::Register) || acceptKw(Kw::Bus)) {
                    }
                    if (accept(Tok::VarAssign))
                        d->init = expression();
                }
                expect(Tok::Semicolon);
                return d;
            }
            case Kw::Type: {
                advance();
                d->kind = DeclKind::Type;
                d->names.push_back(identifier());
                if (accept(Tok::Semicolon)) {
                    d->type = std::make_unique<TypeDef>();
                    d->type->kind = TypeDef::Kind::Incomplete;
                    d->type->loc = d->loc;
                    return d;
                }
                expectKw(Kw::Is);
                d->type = typeDefinition(d->names.front());
                expect(Tok::Semicolon);
                return d;
            }
            case Kw::Subtype:
                advance();
                d->kind = DeclKind::Subtype;
                d->names.push_back(identifier());
                expectKw(Kw::Is);
                d->subtype = subtypeIndication();
                expect(Tok::Semicolon);
                return d;
            case Kw::Alias:
                advance();
                d->kind = DeclKind::Alias;
                if (at(Tok::CharLiteral) || at(Tok::StringLiteral))
                    d->names.push_back(identOf(advance()));
                else
                    d->names.push_back(identifier());
                if (accept(Tok::Colon))
                    d->subtype = subtypeIndication();
                expectKw(Kw::Is);
                d->init = name();
                if (at(Tok::LBracket))
                    skipSignature();
                expect(Tok::Semicolon);
                return d;
            case Kw::Component:
                advance();
                d->kind = DeclKind::Component;
                d->names.push_back(identifier());
                acceptKw(Kw::Is);
                if (atKw(Kw::Generic))
                    d->generics = genericClause();
                if (atKw(Kw::Port))
                    d->ports = portClause();
                expectKw(Kw::End);
                endTail({Kw::Component});
                return d;
            case Kw::Attribute: {
                advance();
                Ident attr = identifier();
                d->names.push_back(attr);
                if (accept(Tok::Colon)) {
                    d->kind = DeclKind::AttributeDecl;
                    d->subtype = subtypeIndication();
                    expect(Tok::Semicolon);
                    return d;
                }
                d->kind = DeclKind::AttributeSpec;
                expectKw(Kw::Of);
                if (atKw(Kw::Others) || atKw(Kw::All)) {
                    Ident id = identOf(advance());
                    d->entityNames.push_back(id);
                }
                else {
                    do {
                        if (at(Tok::CharLiteral) || at(Tok::StringLiteral))
                            d->entityNames.push_back(identOf(advance()));
                        else
                            d->entityNames.push_back(identifier());
                        if (at(Tok::LBracket))
                            skipSignature();
                    } while (accept(Tok::Comma));
                }
                expect(Tok::Colon);
                if (tok().kind == Tok::Keyword || atIdent())
                    d->entityClass = lowerAscii(advance().text);
                else
                    throw err("expected an entity class");
                expectKw(Kw::Is);
                d->init = expression();
                expect(Tok::Semicolon);
                return d;
            }
            case Kw::Function:
            case Kw::Procedure:
            case Kw::Pure:
            case Kw::Impure: {
                d->kind = DeclKind::Subprogram;
                d->sub = subprogramSpec();
                d->names.push_back(d->sub->designator);
                if (acceptKw(Kw::Is)) {
                    if (acceptKw(Kw::New)) { // VHDL-2008 subprogram instantiation
                        d->kind = DeclKind::Ignored;
                        d->what = "subprogram instantiation";
                        skipToSemicolon();
                        return d;
                    }
                    d->sub->hasBody = true;
                    d->sub->decls = declarations();
                    expectKw(Kw::Begin);
                    d->sub->body = sequentialStatements();
                    expectKw(Kw::End);
                    endTail({d->sub->isFunction ? Kw::Function : Kw::Procedure});
                }
                else {
                    expect(Tok::Semicolon);
                }
                return d;
            }
            case Kw::Use:
                advance();
                d->kind = DeclKind::Use;
                d->useNames.push_back(name());
                while (accept(Tok::Comma))
                    d->useNames.push_back(name());
                expect(Tok::Semicolon);
                return d;
            case Kw::Package: {
                advance();
                Ident n = identifier();
                expectKw(Kw::Is);
                if (acceptKw(Kw::New)) {
                    d->kind = DeclKind::PackageInst;
                    d->names.push_back(n);
                    d->unit = name();
                    if (acceptKw(Kw::Generic)) {
                        expectKw(Kw::Map);
                        d->genericMap = associationList();
                    }
                    expect(Tok::Semicolon);
                    return d;
                }
                // nested package declaration (VHDL-2008): skip
                d->kind = DeclKind::Ignored;
                d->what = "nested package";
                declarations();
                expectKw(Kw::End);
                endTail({Kw::Package});
                return d;
            }
            case Kw::Group:
            case Kw::Disconnect:
                d->kind = DeclKind::Ignored;
                d->what = keywordText(tok().kw);
                skipToSemicolon();
                return d;
            case Kw::For: {
                // configuration specification: for L : C use entity ...;
                d->kind = DeclKind::Ignored;
                d->what = "configuration specification";
                skipToSemicolon();
                if (atKw(Kw::End) && atKw(Kw::For, 1)) {
                    advance();
                    advance();
                    expect(Tok::Semicolon);
                }
                return d;
            }
            default: break;
        }
        throw err("expected a declaration but found " + describe(tok()));
    }

    void skipSignature() {
        expect(Tok::LBracket);
        while (!at(Tok::RBracket) && !at(Tok::End))
            advance();
        expect(Tok::RBracket);
    }

    std::unique_ptr<Subprogram> subprogramSpec() {
        auto s = std::make_unique<Subprogram>();
        s->loc = loc();
        if (acceptKw(Kw::Pure))
            s->pure = true;
        else if (acceptKw(Kw::Impure))
            s->pure = false;
        if (acceptKw(Kw::Function))
            s->isFunction = true;
        else
            expectKw(Kw::Procedure);
        if (at(Tok::StringLiteral))
            s->designator = identOf(advance());
        else
            s->designator = identifier();
        if (atKw(Kw::Generic)) {
            advance();
            s->generics = interfaceList(InterfaceDecl::Class::Constant);
            if (acceptKw(Kw::Generic)) {
                expectKw(Kw::Map);
                associationList();
            }
        }
        if (atWord("parameter"))
            advance();
        if (at(Tok::LParen))
            s->params = interfaceList(s->isFunction ? InterfaceDecl::Class::Constant : InterfaceDecl::Class::Default);
        if (s->isFunction) {
            expectKw(Kw::Return);
            s->returnType = name();
        }
        return s;
    }

    std::unique_ptr<TypeDef> typeDefinition(const Ident& typeName) {
        auto t = std::make_unique<TypeDef>();
        t->loc = loc();
        if (accept(Tok::LParen)) {
            t->kind = TypeDef::Kind::Enum;
            do {
                if (at(Tok::CharLiteral))
                    t->literals.push_back(identOf(advance()));
                else
                    t->literals.push_back(identifier());
            } while (accept(Tok::Comma));
            expect(Tok::RParen);
            return t;
        }
        if (acceptKw(Kw::Range)) {
            auto r = rangeConstraintExpr();
            if (acceptKw(Kw::Units)) {
                t->kind = TypeDef::Kind::Physical;
                t->range = std::move(r);
                PhysicalUnit primary;
                primary.name = identifier();
                expect(Tok::Semicolon);
                t->units.push_back(std::move(primary));
                while (!atKw(Kw::End)) {
                    PhysicalUnit u;
                    u.name = identifier();
                    expect(Tok::Eq);
                    u.value = expression();
                    expect(Tok::Semicolon);
                    t->units.push_back(std::move(u));
                }
                expectKw(Kw::End);
                expectKw(Kw::Units);
                if (atIdent())
                    advance();
                return t;
            }
            t->kind = TypeDef::Kind::Range;
            t->range = std::move(r);
            return t;
        }
        if (acceptKw(Kw::Array)) {
            t->kind = TypeDef::Kind::Array;
            expect(Tok::LParen);
            do {
                auto e = discreteRange();
                if (e->kind == ExprKind::Subtype && e->subtype->range && e->subtype->range->kind == ExprKind::Box)
                    t->unconstrained = true;
                t->indexes.push_back(std::move(e));
            } while (accept(Tok::Comma));
            expect(Tok::RParen);
            expectKw(Kw::Of);
            t->element = subtypeIndication();
            return t;
        }
        if (acceptKw(Kw::Record)) {
            t->kind = TypeDef::Kind::Record;
            while (!atKw(Kw::End)) {
                RecordField f;
                f.loc = loc();
                f.names.push_back(identifier());
                while (accept(Tok::Comma))
                    f.names.push_back(identifier());
                expect(Tok::Colon);
                f.subtype = subtypeIndication();
                expect(Tok::Semicolon);
                t->fields.push_back(std::move(f));
            }
            expectKw(Kw::End);
            expectKw(Kw::Record);
            if (atIdent())
                advance();
            return t;
        }
        if (acceptKw(Kw::Access)) {
            t->kind = TypeDef::Kind::Access;
            t->element = subtypeIndication();
            return t;
        }
        if (acceptKw(Kw::File)) {
            t->kind = TypeDef::Kind::File;
            expectKw(Kw::Of);
            t->element = subtypeIndication();
            return t;
        }
        if (acceptKw(Kw::Protected)) {
            t->kind = TypeDef::Kind::Protected;
            const bool body = acceptKw(Kw::Body);
            if (body) {
                t->protectedDecls = declarations();
            }
            else {
                t->protectedDecls = declarations();
            }
            expectKw(Kw::End);
            expectKw(Kw::Protected);
            if (body)
                expectKw(Kw::Body);
            if (atIdent())
                advance();
            return t;
        }
        throw err("expected a type definition for '" + typeName.text + "' but found " + describe(tok()));
    }

    // ------------------------------------------------------------------
    // subtype indications and ranges
    // ------------------------------------------------------------------
    SubtypePtr subtypeIndication() {
        auto s = std::make_unique<SubtypeIndication>();
        s->loc = loc();
        if (at(Tok::LParen)) {
            // VHDL-2008 element resolution: (resolved) std_ulogic_vector
            advance();
            s->resolution = expression();
            expect(Tok::RParen);
        }
        auto first = name();
        if (atIdent()) {
            // resolution function followed by the type mark
            s->resolution = std::move(first);
            first = name();
        }
        s->mark = std::move(first);
        if (acceptKw(Kw::Range)) {
            if (at(Tok::Box)) {
                s->range = std::make_unique<Expr>(ExprKind::Box, loc());
                advance();
            }
            else {
                s->range = rangeConstraintExpr();
            }
        }
        return s;
    }

    /// After "range": l to r | l downto r | x'range
    ExprPtr rangeConstraintExpr() {
        auto l = simpleExpression();
        if (atKw(Kw::To) || atKw(Kw::Downto)) {
            auto r = std::make_unique<Expr>(ExprKind::Range, l->loc);
            r->dir = advance().kw == Kw::To ? RangeDir::To : RangeDir::Downto;
            r->lhs = std::move(l);
            r->rhs = simpleExpression();
            return r;
        }
        return l; // attribute name such as x'range
    }

    /// Discrete range or expression: e | e to/downto e | mark range ... |
    /// mark range <> (index subtype definition).
    ExprPtr discreteRange() {
        auto e = expression();
        if (atKw(Kw::To) || atKw(Kw::Downto)) {
            auto r = std::make_unique<Expr>(ExprKind::Range, e->loc);
            r->dir = advance().kw == Kw::To ? RangeDir::To : RangeDir::Downto;
            r->lhs = std::move(e);
            r->rhs = expression();
            return r;
        }
        if (atKw(Kw::Range)) {
            advance();
            auto s = std::make_unique<Expr>(ExprKind::Subtype, e->loc);
            s->subtype = std::make_unique<SubtypeIndication>();
            s->subtype->loc = e->loc;
            s->subtype->mark = std::move(e);
            if (at(Tok::Box)) {
                s->subtype->range = std::make_unique<Expr>(ExprKind::Box, loc());
                advance();
            }
            else {
                s->subtype->range = rangeConstraintExpr();
            }
            return s;
        }
        if (atIdent() && isNameExpr(*e)) {
            // "resolved std_logic" style subtype in a discrete-range position (rare)
            auto s = std::make_unique<Expr>(ExprKind::Subtype, e->loc);
            s->subtype = std::make_unique<SubtypeIndication>();
            s->subtype->resolution = std::move(e);
            s->subtype->mark = name();
            return s;
        }
        return e;
    }

    static bool isNameExpr(const Expr& e) {
        return e.kind == ExprKind::Simple || e.kind == ExprKind::Selected || e.kind == ExprKind::Call ||
               e.kind == ExprKind::Attribute;
    }

    // ------------------------------------------------------------------
    // names
    // ------------------------------------------------------------------
    ExprPtr name() {
        ExprPtr n;
        const Loc l = loc();
        if (atIdent()) {
            n = std::make_unique<Expr>(ExprKind::Simple, l);
            n->ident = identOf(advance());
        }
        else if (at(Tok::StringLiteral)) { // operator symbol
            n = std::make_unique<Expr>(ExprKind::Simple, l);
            n->ident = identOf(advance());
        }
        else if (at(Tok::CharLiteral)) {
            n = std::make_unique<Expr>(ExprKind::Character, l);
            n->ident = identOf(advance());
        }
        else if (at(Tok::DLt)) {
            n = externalName();
        }
        else {
            throw err("expected a name but found " + describe(tok()));
        }
        return nameSuffixes(std::move(n));
    }

    ExprPtr externalName() {
        auto n = std::make_unique<Expr>(ExprKind::External, loc());
        expect(Tok::DLt);
        while (!at(Tok::Colon) && !at(Tok::End)) {
            n->text += advance().text;
        }
        expect(Tok::Colon);
        n->subtype = subtypeIndication();
        expect(Tok::DGt);
        return n;
    }

    ExprPtr nameSuffixes(ExprPtr n) {
        for (;;) {
            if (at(Tok::Dot)) {
                const Loc l = loc();
                advance();
                auto s = std::make_unique<Expr>(ExprKind::Selected, l);
                if (atKw(Kw::All)) {
                    s->ident = identOf(advance());
                    s->ident.key = "all";
                }
                else if (at(Tok::CharLiteral) || at(Tok::StringLiteral) || atIdent()) {
                    s->ident = identOf(advance());
                }
                else {
                    throw err("expected a suffix after '.' but found " + describe(tok()));
                }
                s->prefix = std::move(n);
                n = std::move(s);
            }
            else if (at(Tok::Tick)) {
                const Loc l = loc();
                if (at(Tok::LParen, 1)) {
                    // qualified expression: mark'(expr) or mark'(aggregate)
                    advance();
                    auto q = std::make_unique<Expr>(ExprKind::Qualified, l);
                    q->prefix = std::move(n);
                    q->lhs = parenthesized();
                    n = std::move(q);
                    continue;
                }
                advance();
                auto a = std::make_unique<Expr>(ExprKind::Attribute, l);
                if (atIdent() || tok().kind == Tok::Keyword) // 'range, 'subtype are keywords
                    a->ident = identOf(advance());
                else
                    throw err("expected an attribute name after ''' but found " + describe(tok()));
                a->prefix = std::move(n);
                n = std::move(a);
                if (at(Tok::LParen) && attributeTakesParameter(n->ident.key)) {
                    advance();
                    Assoc p;
                    p.loc = loc();
                    p.actual = expression();
                    n->args.push_back(std::move(p));
                    expect(Tok::RParen);
                }
            }
            else if (at(Tok::LParen)) {
                const Loc l = loc();
                auto c = std::make_unique<Expr>(ExprKind::Call, l);
                c->args = associationList();
                c->prefix = std::move(n);
                n = std::move(c);
            }
            else if (at(Tok::LBracket)) {
                skipSignature(); // signature in an alias/attribute name
            }
            else {
                return n;
            }
        }
    }

    static bool attributeTakesParameter(const std::string& key) {
        // Attributes whose parameter is written in parentheses directly after
        // the name. Others (e.g. 'length of a slice) never take one.
        static const char* const kWithParam[] = {
            "image", "value",  "pos",      "val",  "succ",   "pred",   "leftof", "rightof", "high",
            "low",   "left",   "right",    "length", "range", "reverse_range", "ascending", "delayed",
            "stable", "quiet", "transaction_unused", "base_unused", "element_unused"};
        return std::ranges::any_of(kWithParam, [&](const char* k) { return key == k; });
    }

    std::vector<Assoc> associationList() {
        expect(Tok::LParen);
        std::vector<Assoc> out;
        do {
            Assoc a;
            a.loc = loc();
            auto first = associationActual();
            if (accept(Tok::Arrow)) {
                a.formal = std::move(first);
                a.actual = associationActual();
            }
            else {
                a.actual = std::move(first);
            }
            out.push_back(std::move(a));
        } while (accept(Tok::Comma));
        expect(Tok::RParen);
        return out;
    }

    ExprPtr associationActual() {
        if (atKw(Kw::Open)) {
            auto e = std::make_unique<Expr>(ExprKind::Open, loc());
            advance();
            return e;
        }
        if (atKw(Kw::Inertial)) {
            auto e = std::make_unique<Expr>(ExprKind::Inertial, loc());
            advance();
            e->lhs = expression();
            return e;
        }
        if (at(Tok::Box)) {
            auto e = std::make_unique<Expr>(ExprKind::Box, loc());
            advance();
            return e;
        }
        return discreteRange();
    }

    // ------------------------------------------------------------------
    // expressions
    // ------------------------------------------------------------------
    static Op logicalOp(const Token& t) {
        if (t.kind != Tok::Keyword)
            return Op::None;
        switch (t.kw) {
            case Kw::And: return Op::And;
            case Kw::Or: return Op::Or;
            case Kw::Nand: return Op::Nand;
            case Kw::Nor: return Op::Nor;
            case Kw::Xor: return Op::Xor;
            case Kw::Xnor: return Op::Xnor;
            default: return Op::None;
        }
    }

    ExprPtr binary(Op op, ExprPtr l, ExprPtr r, Loc opLoc) {
        auto e = std::make_unique<Expr>(ExprKind::Binary, opLoc);
        e->op = op;
        e->lhs = std::move(l);
        e->rhs = std::move(r);
        return e;
    }

    ExprPtr unary(Op op, ExprPtr operand, Loc opLoc) {
        auto e = std::make_unique<Expr>(ExprKind::Unary, opLoc);
        e->op = op;
        e->lhs = std::move(operand);
        return e;
    }

public:
    ExprPtr expression() {
        if (at(Tok::Cond)) {
            const Loc l = loc();
            advance();
            return unary(Op::Condition, primary(), l);
        }
        auto l = relation();
        Op first = Op::None;
        while (logicalOp(tok()) != Op::None) {
            const Op op = logicalOp(tok());
            const Loc opLoc = loc();
            if (first == Op::None)
                first = op;
            else if (op != first || op == Op::Nand || op == Op::Nor)
                throw err("mixing logical operators requires parentheses");
            advance();
            l = binary(op, std::move(l), relation(), opLoc);
        }
        return l;
    }

private:
    static Op relOp(Tok k) {
        switch (k) {
            case Tok::Eq: return Op::Eq;
            case Tok::Ne: return Op::Ne;
            case Tok::Lt: return Op::Lt;
            case Tok::Le: return Op::Le;
            case Tok::Gt: return Op::Gt;
            case Tok::Ge: return Op::Ge;
            case Tok::MatchEq: return Op::MatchEq;
            case Tok::MatchNe: return Op::MatchNe;
            case Tok::MatchLt: return Op::MatchLt;
            case Tok::MatchLe: return Op::MatchLe;
            case Tok::MatchGt: return Op::MatchGt;
            case Tok::MatchGe: return Op::MatchGe;
            default: return Op::None;
        }
    }

    ExprPtr relation() {
        auto l = shiftExpression();
        const Op op = relOp(tok().kind);
        if (op != Op::None) {
            const Loc opLoc = loc();
            advance();
            l = binary(op, std::move(l), shiftExpression(), opLoc);
        }
        return l;
    }

    ExprPtr shiftExpression() {
        auto l = simpleExpression();
        if (tok().kind == Tok::Keyword) {
            Op op = Op::None;
            switch (tok().kw) {
                case Kw::Sll: op = Op::Sll; break;
                case Kw::Srl: op = Op::Srl; break;
                case Kw::Sla: op = Op::Sla; break;
                case Kw::Sra: op = Op::Sra; break;
                case Kw::Rol: op = Op::Rol; break;
                case Kw::Ror: op = Op::Ror; break;
                default: break;
            }
            if (op != Op::None) {
                const Loc opLoc = loc();
                advance();
                l = binary(op, std::move(l), simpleExpression(), opLoc);
            }
        }
        return l;
    }

    ExprPtr simpleExpression() {
        ExprPtr l;
        if (at(Tok::Plus) || at(Tok::Minus)) {
            const Loc opLoc = loc();
            const Op op = advance().kind == Tok::Plus ? Op::Plus : Op::Minus;
            l = unary(op, term(), opLoc);
        }
        else {
            l = term();
        }
        for (;;) {
            Op op = Op::None;
            if (at(Tok::Plus))
                op = Op::Add;
            else if (at(Tok::Minus))
                op = Op::Sub;
            else if (at(Tok::Amp))
                op = Op::Concat;
            if (op == Op::None)
                return l;
            const Loc opLoc = loc();
            advance();
            l = binary(op, std::move(l), term(), opLoc);
        }
    }

    ExprPtr term() {
        auto l = factor();
        for (;;) {
            Op op = Op::None;
            if (at(Tok::Star))
                op = Op::Mul;
            else if (at(Tok::Slash))
                op = Op::Div;
            else if (atKw(Kw::Mod))
                op = Op::Mod;
            else if (atKw(Kw::Rem))
                op = Op::Rem;
            if (op == Op::None)
                return l;
            const Loc opLoc = loc();
            advance();
            l = binary(op, std::move(l), factor(), opLoc);
        }
    }

    ExprPtr factor() {
        const Loc opLoc = loc();
        if (acceptKw(Kw::Abs))
            return unary(Op::Abs, primary(), opLoc);
        if (acceptKw(Kw::Not))
            return unary(Op::Not, primary(), opLoc);
        // VHDL-2008 unary reduction operators
        if (const Op lop = logicalOp(tok()); lop != Op::None) {
            advance();
            static constexpr std::pair<Op, Op> kRed[] = {{Op::And, Op::RedAnd},   {Op::Or, Op::RedOr},
                                                         {Op::Nand, Op::RedNand}, {Op::Nor, Op::RedNor},
                                                         {Op::Xor, Op::RedXor},   {Op::Xnor, Op::RedXnor}};
            Op red = Op::RedAnd;
            for (const auto& [a, b] : kRed)
                if (a == lop)
                    red = b;
            return unary(red, primary(), opLoc);
        }
        auto l = primary();
        if (at(Tok::DoubleStar)) {
            const Loc pl = loc();
            advance();
            l = binary(Op::Pow, std::move(l), primary(), pl);
        }
        return l;
    }

    ExprPtr primary() {
        const Loc l = loc();
        switch (tok().kind) {
            case Tok::AbstractLiteral: {
                auto e = std::make_unique<Expr>(ExprKind::Integer, l);
                e->text = advance().text;
                literalValue(*e, e->text);
                if (atIdent()) {
                    // physical literal: 10 ns
                    auto p = std::make_unique<Expr>(ExprKind::Physical, l);
                    p->ident = identOf(advance());
                    p->lhs = std::move(e);
                    return p;
                }
                return e;
            }
            case Tok::CharLiteral: {
                auto e = std::make_unique<Expr>(ExprKind::Character, l);
                e->ident = identOf(advance());
                return nameSuffixes(std::move(e));
            }
            case Tok::StringLiteral: {
                if (at(Tok::LParen, 1)) // operator symbol call: "and"(a, b)
                    return name();
                auto e = std::make_unique<Expr>(ExprKind::String, l);
                e->text = advance().text;
                return e;
            }
            case Tok::BitStringLiteral: {
                auto e = std::make_unique<Expr>(ExprKind::BitString, l);
                const Token& t = advance();
                e->text = t.text;
                // explicit VHDL-2008 length
                std::size_t k = 0;
                std::uint32_t n = 0;
                while (k < t.value.size() && t.value[k] >= '0' && t.value[k] <= '9')
                    n = n * 10 + static_cast<std::uint32_t>(t.value[k++] - '0');
                e->bitWidth = k > 0 ? n : 0;
                return e;
            }
            case Tok::LParen: return parenthesized();
            case Tok::Identifier:
            case Tok::ExtIdentifier:
            case Tok::DLt: return name();
            case Tok::Keyword:
                if (acceptKw(Kw::Null))
                    return std::make_unique<Expr>(ExprKind::Null, l);
                if (acceptKw(Kw::New)) {
                    // allocator: not synthesizable; keep the designated subtype
                    auto e = std::make_unique<Expr>(ExprKind::Subtype, l);
                    e->subtype = subtypeIndication();
                    e->text = "new";
                    return e;
                }
                break;
            default: break;
        }
        throw err("expected an expression but found " + describe(tok()));
    }

    /// "(" expression ")" or an aggregate.
    ExprPtr parenthesized() {
        const Loc l = loc();
        expect(Tok::LParen);
        std::vector<ElementAssoc> elems;
        bool named = false;
        do {
            ElementAssoc ea;
            ea.loc = loc();
            std::vector<ExprPtr> choices;
            choices.push_back(choice());
            while (accept(Tok::Bar))
                choices.push_back(choice());
            if (accept(Tok::Arrow)) {
                named = true;
                ea.choices = std::move(choices);
                ea.value = expression();
            }
            else {
                if (choices.size() != 1)
                    throw err("expected '=>' after a choice list");
                ea.value = std::move(choices.front());
                if (ea.value->kind == ExprKind::Others || ea.value->kind == ExprKind::Range)
                    throw err("expected '=>' after a choice");
            }
            elems.push_back(std::move(ea));
        } while (accept(Tok::Comma));
        expect(Tok::RParen);
        if (elems.size() == 1 && !named) {
            auto e = std::move(elems.front().value);
            e->parenthesized = true;
            return e;
        }
        auto a = std::make_unique<Expr>(ExprKind::Aggregate, l);
        a->elements = std::move(elems);
        return a;
    }

    /// Choice: others | discrete range | expression (also element names).
    ExprPtr choice() {
        if (atKw(Kw::Others)) {
            auto e = std::make_unique<Expr>(ExprKind::Others, loc());
            advance();
            return e;
        }
        return discreteRange();
    }

    std::vector<ExprPtr> choices() {
        std::vector<ExprPtr> out;
        out.push_back(choice());
        while (accept(Tok::Bar))
            out.push_back(choice());
        return out;
    }

    // ------------------------------------------------------------------
    // sequential statements
    // ------------------------------------------------------------------
    [[nodiscard]] bool atSequentialEnd() const {
        return atKw(Kw::End) || atKw(Kw::Elsif) || atKw(Kw::Else) || atKw(Kw::When) || at(Tok::End);
    }

    std::vector<StmtPtr> sequentialStatements() {
        std::vector<StmtPtr> out;
        while (!atSequentialEnd()) {
            const std::size_t start = p_;
            try {
                out.push_back(sequentialStatement());
            }
            catch (const ParseError& e) {
                error(e);
                if (p_ == start)
                    advance();
                skipToSemicolon();
            }
        }
        return out;
    }

    Ident optionalLabel() {
        if (atIdent() && at(Tok::Colon, 1)) {
            Ident l = identOf(advance());
            advance();
            return l;
        }
        return {};
    }

    StmtPtr sequentialStatement() {
        auto s = std::make_unique<Stmt>();
        s->label = optionalLabel();
        s->loc = s->label.empty() ? loc() : s->label.loc;
        const Loc l = loc();
        if (acceptKw(Kw::If)) {
            s->kind = StmtKind::If;
            IfBranch b;
            b.loc = l;
            b.cond = expression();
            expectKw(Kw::Then);
            b.body = sequentialStatements();
            s->branches.push_back(std::move(b));
            while (atKw(Kw::Elsif)) {
                IfBranch e;
                e.loc = loc();
                advance();
                e.cond = expression();
                expectKw(Kw::Then);
                e.body = sequentialStatements();
                s->branches.push_back(std::move(e));
            }
            if (atKw(Kw::Else)) {
                IfBranch e;
                e.loc = loc();
                advance();
                e.body = sequentialStatements();
                s->branches.push_back(std::move(e));
            }
            expectKw(Kw::End);
            expectKw(Kw::If);
            if (atIdent())
                advance();
            expect(Tok::Semicolon);
            return s;
        }
        if (acceptKw(Kw::Case)) {
            s->kind = StmtKind::Case;
            if (accept(Tok::Question))
                s->matching = true;
            s->selector = expression();
            expectKw(Kw::Is);
            while (atKw(Kw::When)) {
                CaseAlt a;
                a.loc = loc();
                advance();
                a.choices = choices();
                expect(Tok::Arrow);
                a.body = sequentialStatements();
                s->caseAlts.push_back(std::move(a));
            }
            expectKw(Kw::End);
            expectKw(Kw::Case);
            accept(Tok::Question);
            if (atIdent())
                advance();
            expect(Tok::Semicolon);
            return s;
        }
        if (atKw(Kw::While) || atKw(Kw::For) || atKw(Kw::Loop)) {
            s->kind = StmtKind::Loop;
            if (acceptKw(Kw::While)) {
                s->loopKind = Stmt::LoopKind::While;
                s->cond = expression();
            }
            else if (acceptKw(Kw::For)) {
                s->loopKind = Stmt::LoopKind::For;
                s->loopVar = identifier();
                expectKw(Kw::In);
                s->range = discreteRange();
            }
            expectKw(Kw::Loop);
            s->body = sequentialStatements();
            expectKw(Kw::End);
            expectKw(Kw::Loop);
            if (atIdent())
                advance();
            expect(Tok::Semicolon);
            return s;
        }
        if (atKw(Kw::Next) || atKw(Kw::Exit)) {
            s->kind = advance().kw == Kw::Next ? StmtKind::Next : StmtKind::Exit;
            if (atIdent())
                s->loopLabel = identifier();
            if (acceptKw(Kw::When))
                s->cond = expression();
            expect(Tok::Semicolon);
            return s;
        }
        if (acceptKw(Kw::Return)) {
            s->kind = StmtKind::Return;
            if (!at(Tok::Semicolon))
                s->value = expression();
            expect(Tok::Semicolon);
            return s;
        }
        if (acceptKw(Kw::Null)) {
            s->kind = StmtKind::Null;
            expect(Tok::Semicolon);
            return s;
        }
        if (acceptKw(Kw::Wait)) {
            s->kind = StmtKind::Wait;
            if (acceptKw(Kw::On)) {
                s->sensitivity.push_back(name());
                while (accept(Tok::Comma))
                    s->sensitivity.push_back(name());
            }
            if (acceptKw(Kw::Until))
                s->cond = expression();
            if (acceptKw(Kw::For))
                s->value = expression();
            expect(Tok::Semicolon);
            return s;
        }
        if (acceptKw(Kw::Assert)) {
            s->kind = StmtKind::Assert;
            s->cond = expression();
            if (acceptKw(Kw::Report))
                s->value = expression();
            if (acceptKw(Kw::Severity))
                s->severity = expression();
            expect(Tok::Semicolon);
            return s;
        }
        if (acceptKw(Kw::Report)) {
            s->kind = StmtKind::Report;
            s->value = expression();
            if (acceptKw(Kw::Severity))
                s->severity = expression();
            expect(Tok::Semicolon);
            return s;
        }
        if (acceptKw(Kw::With)) {
            // VHDL-2008 sequential selected assignment
            s->selector = expression();
            expectKw(Kw::Select);
            if (accept(Tok::Question))
                s->matching = true;
            s->target = target();
            selectedAssignment(*s, /*concurrent=*/false);
            return s;
        }
        // assignment or procedure call
        s->target = target();
        if (accept(Tok::Le)) {
            s->kind = StmtKind::SignalAssign;
            signalAssignmentRhs(*s, /*concurrent=*/false);
            return s;
        }
        if (accept(Tok::VarAssign)) {
            s->kind = StmtKind::VariableAssign;
            variableAssignmentRhs(*s);
            return s;
        }
        if (accept(Tok::Semicolon)) {
            s->kind = StmtKind::ProcCall;
            s->call = std::move(s->target);
            return s;
        }
        throw err("expected '<=', ':=' or ';' after " + describeExpr(*s->target) + " but found " + describe(tok()));
    }

    static std::string describeExpr(const Expr& e) {
        if (e.kind == ExprKind::Simple)
            return "'" + e.ident.text + "'";
        if (e.prefix)
            return describeExpr(*e.prefix);
        return "the target";
    }

    ExprPtr target() {
        if (at(Tok::LParen))
            return parenthesized(); // aggregate target
        return name();
    }

    void delayMechanism(Stmt& s) {
        if (acceptKw(Kw::Transport)) {
            s.delayMechanism = "transport";
        }
        else if (acceptKw(Kw::Reject)) {
            expression();
            expectKw(Kw::Inertial);
            s.delayMechanism = "reject";
        }
        else if (acceptKw(Kw::Inertial)) {
            s.delayMechanism = "inertial";
        }
        if (atWord("force") || atWord("release")) {
            warning("force/release assignments are treated as ordinary assignments", loc());
            advance();
            if (atKw(Kw::In) || atKw(Kw::Out))
                advance();
        }
    }

    std::vector<WaveElem> waveform(bool& unaffected) {
        std::vector<WaveElem> w;
        if (acceptKw(Kw::Unaffected)) {
            unaffected = true;
            return w;
        }
        do {
            WaveElem e;
            if (atKw(Kw::Null) && !at(Tok::Semicolon, 1)) {
                advance();
            }
            else {
                e.value = expression();
            }
            if (acceptKw(Kw::After))
                e.after = expression();
            w.push_back(std::move(e));
        } while (accept(Tok::Comma));
        return w;
    }

    /// After "<=": [guarded] [delay] waveform [when cond else waveform ...] ;
    void signalAssignmentRhs(Stmt& s, bool concurrent) {
        if (concurrent && acceptKw(Kw::Guarded))
            s.guarded = true;
        delayMechanism(s);
        for (;;) {
            AssignAlt a;
            a.loc = loc();
            a.wave = waveform(a.unaffected);
            if (acceptKw(Kw::When)) {
                s.form = Stmt::Form::Conditional;
                a.cond = expression();
                s.alts.push_back(std::move(a));
                if (acceptKw(Kw::Else))
                    continue;
                break;
            }
            s.alts.push_back(std::move(a));
            break;
        }
        expect(Tok::Semicolon);
    }

    void variableAssignmentRhs(Stmt& s) {
        for (;;) {
            AssignAlt a;
            a.loc = loc();
            WaveElem w;
            w.value = expression();
            a.wave.push_back(std::move(w));
            if (acceptKw(Kw::When)) {
                s.form = Stmt::Form::Conditional;
                a.cond = expression();
                s.alts.push_back(std::move(a));
                if (acceptKw(Kw::Else))
                    continue;
                break;
            }
            s.alts.push_back(std::move(a));
            break;
        }
        expect(Tok::Semicolon);
    }

    /// After "with sel select [?] target": <= / := alternatives
    void selectedAssignment(Stmt& s, bool concurrent) {
        s.form = Stmt::Form::Selected;
        const bool variable = !concurrent && at(Tok::VarAssign);
        if (variable) {
            advance();
            s.kind = StmtKind::VariableAssign;
        }
        else {
            expect(Tok::Le);
            s.kind = concurrent ? StmtKind::ConcAssign : StmtKind::SignalAssign;
            if (concurrent && acceptKw(Kw::Guarded))
                s.guarded = true;
            delayMechanism(s);
        }
        do {
            AssignAlt a;
            a.loc = loc();
            if (variable) {
                WaveElem w;
                w.value = expression();
                a.wave.push_back(std::move(w));
            }
            else {
                a.wave = waveform(a.unaffected);
            }
            expectKw(Kw::When);
            a.choices = choices();
            s.alts.push_back(std::move(a));
        } while (accept(Tok::Comma));
        expect(Tok::Semicolon);
    }

    // ------------------------------------------------------------------
    // concurrent statements
    // ------------------------------------------------------------------
    std::vector<StmtPtr> concurrentStatements() {
        std::vector<StmtPtr> out;
        while (!atKw(Kw::End) && !atKw(Kw::Elsif) && !atKw(Kw::Else) && !atKw(Kw::When) && !at(Tok::End)) {
            const std::size_t start = p_;
            try {
                out.push_back(concurrentStatement());
            }
            catch (const ParseError& e) {
                error(e);
                if (p_ == start)
                    advance();
                skipToSemicolon();
            }
        }
        return out;
    }

    /// Generate body: [declarations begin] statements [end [alt_label];]
    void generateBody(std::vector<DeclPtr>& decls, std::vector<StmtPtr>& body) {
        if (atDeclarationStart() || atKw(Kw::Begin)) {
            decls = declarations();
            expectKw(Kw::Begin);
        }
        body = concurrentStatements();
        // VHDL-2008: "end [alternative_label];" closes a generate alternative
        if (atKw(Kw::End) && !atKw(Kw::Generate, 1)) {
            advance();
            if (atIdent())
                advance();
            expect(Tok::Semicolon);
        }
    }

    Ident alternativeLabel() {
        if (atIdent() && at(Tok::Colon, 1)) {
            Ident l = identOf(advance());
            advance();
            return l;
        }
        return {};
    }

    StmtPtr concurrentStatement() {
        auto s = std::make_unique<Stmt>();
        s->label = optionalLabel();
        s->loc = s->label.empty() ? loc() : s->label.loc;
        if (acceptKw(Kw::Postponed))
            s->postponed = true;

        if (acceptKw(Kw::Process)) {
            s->kind = StmtKind::Process;
            if (accept(Tok::LParen)) {
                if (acceptKw(Kw::All)) {
                    s->sensitivityAll = true;
                }
                else {
                    s->sensitivity.push_back(name());
                    while (accept(Tok::Comma))
                        s->sensitivity.push_back(name());
                }
                expect(Tok::RParen);
            }
            acceptKw(Kw::Is);
            s->decls = declarations();
            expectKw(Kw::Begin);
            s->body = sequentialStatements();
            expectKw(Kw::End);
            acceptKw(Kw::Postponed);
            expectKw(Kw::Process);
            if (atIdent())
                advance();
            expect(Tok::Semicolon);
            return s;
        }
        if (acceptKw(Kw::Block)) {
            s->kind = StmtKind::Block;
            if (accept(Tok::LParen)) {
                s->cond = expression();
                expect(Tok::RParen);
            }
            acceptKw(Kw::Is);
            if (atKw(Kw::Generic)) {
                s->generics = genericClause();
                if (acceptKw(Kw::Generic)) {
                    expectKw(Kw::Map);
                    s->genericMap = associationList();
                    expect(Tok::Semicolon);
                }
            }
            if (atKw(Kw::Port)) {
                s->ports = portClause();
                if (acceptKw(Kw::Port)) {
                    expectKw(Kw::Map);
                    s->portMap = associationList();
                    expect(Tok::Semicolon);
                }
            }
            s->decls = declarations();
            expectKw(Kw::Begin);
            s->body = concurrentStatements();
            expectKw(Kw::End);
            expectKw(Kw::Block);
            if (atIdent())
                advance();
            expect(Tok::Semicolon);
            return s;
        }
        if (acceptKw(Kw::For)) {
            s->kind = StmtKind::ForGenerate;
            s->loopVar = identifier();
            expectKw(Kw::In);
            s->range = discreteRange();
            expectKw(Kw::Generate);
            generateBody(s->decls, s->body);
            expectKw(Kw::End);
            expectKw(Kw::Generate);
            if (atIdent())
                advance();
            expect(Tok::Semicolon);
            return s;
        }
        if (atKw(Kw::If) && !s->label.empty()) {
            advance();
            s->kind = StmtKind::IfGenerate;
            IfBranch b;
            b.loc = s->loc;
            b.altLabel = alternativeLabel();
            b.cond = expression();
            expectKw(Kw::Generate);
            generateBody(b.decls, b.body);
            s->branches.push_back(std::move(b));
            while (atKw(Kw::Elsif)) {
                IfBranch e;
                e.loc = loc();
                advance();
                e.altLabel = alternativeLabel();
                e.cond = expression();
                expectKw(Kw::Generate);
                generateBody(e.decls, e.body);
                s->branches.push_back(std::move(e));
            }
            if (atKw(Kw::Else)) {
                IfBranch e;
                e.loc = loc();
                advance();
                e.altLabel = alternativeLabel();
                expectKw(Kw::Generate);
                generateBody(e.decls, e.body);
                s->branches.push_back(std::move(e));
            }
            expectKw(Kw::End);
            expectKw(Kw::Generate);
            if (atIdent())
                advance();
            expect(Tok::Semicolon);
            return s;
        }
        if (atKw(Kw::Case) && !s->label.empty()) {
            advance();
            s->kind = StmtKind::CaseGenerate;
            s->selector = expression();
            expectKw(Kw::Generate);
            while (atKw(Kw::When)) {
                CaseAlt a;
                a.loc = loc();
                advance();
                a.altLabel = alternativeLabel();
                a.choices = choices();
                expect(Tok::Arrow);
                generateBody(a.decls, a.body);
                s->caseAlts.push_back(std::move(a));
            }
            expectKw(Kw::End);
            expectKw(Kw::Generate);
            if (atIdent())
                advance();
            expect(Tok::Semicolon);
            return s;
        }
        if (acceptKw(Kw::Assert)) {
            s->kind = StmtKind::ConcAssert;
            s->cond = expression();
            if (acceptKw(Kw::Report))
                s->value = expression();
            if (acceptKw(Kw::Severity))
                s->severity = expression();
            expect(Tok::Semicolon);
            return s;
        }
        if (acceptKw(Kw::With)) {
            s->selector = expression();
            expectKw(Kw::Select);
            if (accept(Tok::Question))
                s->matching = true;
            s->target = target();
            selectedAssignment(*s, /*concurrent=*/true);
            return s;
        }
        if (atKw(Kw::Component) || atKw(Kw::Entity) || atKw(Kw::Configuration)) {
            s->kind = StmtKind::Instance;
            const Kw k = advance().kw;
            s->unitKind = k == Kw::Component ? Stmt::UnitKind::Component
                          : k == Kw::Entity  ? Stmt::UnitKind::Entity
                                             : Stmt::UnitKind::Configuration;
            s->unit = simpleOrSelectedName();
            if (s->unitKind == Stmt::UnitKind::Entity && accept(Tok::LParen)) {
                s->architecture = identifier();
                expect(Tok::RParen);
            }
            instanceMaps(*s);
            return s;
        }
        // PSL directives (assert always ..., cover ...) written without a
        // leading "--psl" are not analyzed.
        if (atWord("cover") || atWord("assume") || atWord("restrict")) {
            s->kind = StmtKind::Ignored;
            s->what = "PSL directive";
            skipToSemicolon();
            return s;
        }

        // component instantiation (label : name [generic map] [port map];),
        // concurrent signal assignment or concurrent procedure call
        auto tgt = target();
        if (!s->label.empty() && (atKw(Kw::Generic) || atKw(Kw::Port) ||
                                  (at(Tok::Semicolon) && (tgt->kind == ExprKind::Simple ||
                                                          tgt->kind == ExprKind::Selected)))) {
            s->kind = StmtKind::Instance;
            s->unitKind = Stmt::UnitKind::Component;
            s->unit = std::move(tgt);
            instanceMaps(*s);
            return s;
        }
        if (accept(Tok::Le)) {
            s->kind = StmtKind::ConcAssign;
            s->target = std::move(tgt);
            signalAssignmentRhs(*s, /*concurrent=*/true);
            return s;
        }
        if (accept(Tok::Semicolon)) {
            s->kind = StmtKind::ConcProcCall;
            s->call = std::move(tgt);
            return s;
        }
        throw err("expected a concurrent statement but found " + describe(tok()));
    }

    ExprPtr simpleOrSelectedName() {
        auto n = std::make_unique<Expr>(ExprKind::Simple, loc());
        n->ident = identifier();
        while (at(Tok::Dot)) {
            const Loc l = loc();
            advance();
            auto s = std::make_unique<Expr>(ExprKind::Selected, l);
            s->ident = identifier();
            s->prefix = std::move(n);
            n = std::move(s);
        }
        return n;
    }

    void instanceMaps(Stmt& s) {
        if (acceptKw(Kw::Generic)) {
            expectKw(Kw::Map);
            s.genericMap = associationList();
        }
        if (acceptKw(Kw::Port)) {
            expectKw(Kw::Map);
            s.portMap = associationList();
        }
        expect(Tok::Semicolon);
    }

    std::vector<Token> t_;
    std::size_t p_ = 0;
    std::vector<ParseMessage>& msgs_;
};

} // namespace

ParseResult parseVhdl(std::string_view text, std::uint32_t fileIndex) {
    ParseResult r;
    auto lexed = lex(text, fileIndex);
    for (auto& e : lexed.errors)
        r.messages.push_back({std::move(e.message), e.loc, true});
    Parser p(std::move(lexed.tokens), r.messages);
    r.file = p.designFile();
    r.file.file = fileIndex;
    r.file.translateOffRegions = lexed.translateOffRegions;
    std::ranges::stable_sort(r.messages, [](const ParseMessage& a, const ParseMessage& b) {
        return a.loc.line != b.loc.line ? a.loc.line < b.loc.line : a.loc.col < b.loc.col;
    });
    return r;
}

ExprPtr parseVhdlExpression(std::string_view text, std::vector<ParseMessage>& messages) {
    auto lexed = lex(text, 0);
    for (auto& e : lexed.errors)
        messages.push_back({std::move(e.message), e.loc, true});
    Parser p(std::move(lexed.tokens), messages);
    return p.singleExpression();
}

} // namespace opencdc::vhdl
