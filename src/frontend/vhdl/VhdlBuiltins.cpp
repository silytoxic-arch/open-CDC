// Predefined and IEEE packages, implemented natively: std.standard,
// std.textio, std.env, ieee.std_logic_1164, ieee.numeric_std,
// ieee.numeric_bit, ieee.numeric_std_unsigned, ieee.std_logic_unsigned /
// std_logic_signed / std_logic_arith / std_logic_misc (Synopsys) and
// ieee.math_real (constant evaluation). No IEEE source text is used: the
// declarations below give the subprogram profiles, callBuiltin() their
// meaning for synthesis.

#include "frontend/vhdl/VhdlElab.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace opencdc::vhdl {

namespace {

constexpr std::int64_t kIntMin = -2147483647LL - 1;
constexpr std::int64_t kIntMax = 2147483647LL;

struct Pkg {
    Elaborator& E;
    PackageInfo* P;

    Symbol* typeSym(const std::string& key, const Type* t) {
        Symbol* s = E.newSymbol(SymKind::Type, key, t->name, {});
        s->type = t;
        P->scope.add(s);
        return s;
    }
    const Type* enumType(const std::string& name, TypeTag tag, const std::vector<std::string>& lits) {
        Type t;
        t.kind = TypeKind::Enum;
        t.tag = tag;
        t.name = name;
        auto info = std::make_shared<EnumInfo>();
        for (const auto& l : lits) {
            info->display.push_back(l);
            info->keys.push_back(l.size() == 3 && l.front() == '\'' ? l : lowerKey(l));
        }
        t.enumInfo = info;
        t.constrained = true;
        t.range = {0, static_cast<std::int64_t>(lits.size()) - 1, true};
        const Type* tp = E.types.make(std::move(t));
        for (std::size_t k = 0; k < info->keys.size(); ++k) {
            Symbol* s = E.newSymbol(SymKind::EnumLiteral, info->keys[k], info->display[k], {});
            s->type = tp;
            s->pos = static_cast<std::int64_t>(k);
            P->scope.add(s);
        }
        typeSym(lowerKey(name), tp);
        return tp;
    }
    static std::string lowerKey(const std::string& s) {
        std::string k = s;
        for (auto& c : k)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        return k;
    }
    const Type* arrayType(const std::string& name, TypeTag tag, const Type* elem, const Type* index) {
        Type t;
        t.kind = TypeKind::Array;
        t.tag = tag;
        t.name = name;
        t.indexTypes = {index};
        t.indexRanges = {std::nullopt};
        t.element = elem;
        const Type* tp = E.types.make(std::move(t));
        typeSym(lowerKey(name), tp);
        return tp;
    }
    const Type* subtype(const std::string& name, const Type* base, IntRange r) {
        const Type* t = E.types.rangeSubtype(base, r, name);
        typeSym(lowerKey(name), t);
        return t;
    }
    const Type* alias(const std::string& name, const Type* t, bool resolved = false) {
        Type copy = *t;
        copy.name = name;
        copy.base = &t->baseType();
        copy.isSubtype = true;
        copy.resolved = resolved;
        const Type* tp = E.types.make(std::move(copy));
        typeSym(lowerKey(name), tp);
        return tp;
    }
    Symbol* fn(const std::string& name, Builtin b, std::vector<const Type*> params, const Type* ret,
               std::vector<std::string> pnames = {}) {
        const bool op = !name.empty() && !std::isalpha(static_cast<unsigned char>(name[0])) && name[0] != '_';
        const std::string key = op || isWordOperator(name) ? "\"" + name + "\"" : lowerKey(name);
        Symbol* s = E.newSymbol(SymKind::Subprogram, key, name, {});
        s->isFunction = ret != nullptr;
        s->type = ret;
        s->builtin = b;
        s->library = P->library + "." + P->name; // package of the builtin
        s->paramsResolved = true;
        for (std::size_t k = 0; k < params.size(); ++k) {
            ParamInfo p;
            p.key = k < pnames.size() ? pnames[k] : (params.size() == 1 ? "arg" : k == 0 ? "l" : k == 1 ? "r" : "p" + std::to_string(k));
            p.display = p.key;
            p.type = params[k];
            p.cls = InterfaceDecl::Class::Constant;
            p.mode = Mode::In;
            s->params.push_back(std::move(p));
        }
        P->scope.add(s);
        return s;
    }
    static bool isWordOperator(const std::string& n) {
        static const char* const ops[] = {"and", "or",  "nand", "nor", "xor", "xnor", "not", "abs", "mod",
                                          "rem", "sll", "srl",  "sla", "sra", "rol",  "ror"};
        return std::ranges::any_of(ops, [&](const char* o) { return n == o; });
    }
    Symbol* constant(const std::string& name, const Type* t, Value v) {
        Symbol* s = E.newSymbol(SymKind::Constant, lowerKey(name), name, {});
        s->type = t;
        s->value = E.makeConst(t, std::move(v));
        P->scope.add(s);
        return s;
    }
};

} // namespace

bool Elaborator::isBuiltinPackage(const std::string& lib, const std::string& name) const {
    if (name.empty())
        return lib == "std" || lib == "ieee";
    return packages.contains({lib, name}) && packages.at({lib, name})->builtin;
}

PackageInfo* Elaborator::builtinPackage(const std::string& lib, const std::string& name) {
    auto it = packages.find({lib, name});
    if (it != packages.end() && it->second->builtin)
        return it->second.get();
    return nullptr;
}

void Elaborator::createBuiltins() {
    auto newPkg = [&](const std::string& lib, const std::string& name) {
        auto p = std::make_unique<PackageInfo>();
        p->library = lib;
        p->name = name;
        p->builtin = true;
        p->elaborated = true;
        p->scope.parent = root;
        p->scope.workLibrary = lib;
        p->scope.label = lib + "." + name;
        PackageInfo* raw = p.get();
        packages[{lib, name}] = std::move(p);
        return raw;
    };

    // ---------------- std.standard ----------------
    PackageInfo* standard = newPkg("std", "standard");
    standardScope = &standard->scope;
    Pkg S{*this, standard};
    std_.boolean = S.enumType("boolean", TypeTag::Boolean, {"false", "true"});
    std_.bit = S.enumType("bit", TypeTag::Bit, {"'0'", "'1'"});
    {
        static const char* const ctrl[] = {"nul", "soh", "stx", "etx", "eot", "enq", "ack", "bel", "bs",  "ht", "lf",
                                           "vt",  "ff",  "cr",  "so",  "si",  "dle", "dc1", "dc2", "dc3", "dc4", "nak",
                                           "syn", "etb", "can", "em",  "sub", "esc", "fsp", "gsp", "rsp", "usp"};
        std::vector<std::string> lits;
        for (int c = 0; c < 256; ++c) {
            if (c < 32)
                lits.emplace_back(ctrl[c]);
            else if (c == 127)
                lits.emplace_back("del");
            else if (c >= 128 && c < 160)
                lits.emplace_back("c" + std::to_string(c));
            else
                lits.push_back(std::string("'") + static_cast<char>(c) + "'");
        }
        std_.character = S.enumType("character", TypeTag::Character, lits);
    }
    std_.severity = S.enumType("severity_level", TypeTag::SeverityLevel, {"note", "warning", "error", "failure"});
    {
        Type t;
        t.kind = TypeKind::Integer;
        t.tag = TypeTag::Integer;
        t.name = "integer";
        t.constrained = true;
        t.range = {kIntMin, kIntMax, true};
        std_.integer = types.make(std::move(t));
        S.typeSym("integer", std_.integer);
        Type u;
        u.kind = TypeKind::UniversalInteger;
        u.name = "universal_integer";
        u.constrained = true;
        u.range = {INT64_MIN, INT64_MAX, true};
        std_.univInt = types.make(std::move(u));
        Type ur;
        ur.kind = TypeKind::UniversalReal;
        ur.name = "universal_real";
        std_.univReal = types.make(std::move(ur));
        Type r;
        r.kind = TypeKind::Real;
        r.tag = TypeTag::Real;
        r.name = "real";
        r.constrained = true;
        r.rrange = {-1.7976931348623157e308, 1.7976931348623157e308, true};
        std_.real = types.make(std::move(r));
        S.typeSym("real", std_.real);
        Type tm;
        tm.kind = TypeKind::Physical;
        tm.tag = TypeTag::Time;
        tm.name = "time";
        tm.constrained = true;
        tm.range = {INT64_MIN, INT64_MAX, true};
        auto units = std::make_shared<std::vector<PhysicalUnitInfo>>();
        const std::pair<const char*, std::int64_t> us[] = {{"fs", 1},
                                                            {"ps", 1000},
                                                            {"ns", 1000000},
                                                            {"us", 1000000000},
                                                            {"ms", 1000000000000LL},
                                                            {"sec", 1000000000000000LL},
                                                            {"min", 60000000000000000LL},
                                                            {"hr", 3600000000000000000LL}};
        for (const auto& [n, m] : us)
            units->push_back({n, m});
        tm.units = units;
        std_.time = types.make(std::move(tm));
        S.typeSym("time", std_.time);
        for (const auto& [n, m] : us) {
            Symbol* s = newSymbol(SymKind::Unit, n, n, {});
            s->type = std_.time;
            s->pos = m;
            standard->scope.add(s);
        }
        S.subtype("delay_length", std_.time, {0, INT64_MAX, true});
    }
    std_.natural = S.subtype("natural", std_.integer, {0, kIntMax, true});
    std_.positive = S.subtype("positive", std_.integer, {1, kIntMax, true});
    std_.string = S.arrayType("string", TypeTag::String, std_.character, std_.positive);
    std_.bitVector = S.arrayType("bit_vector", TypeTag::BitVector, std_.bit, std_.natural);
    std_.booleanVector = S.arrayType("boolean_vector", TypeTag::BooleanVector, std_.boolean, std_.natural);
    std_.integerVector = S.arrayType("integer_vector", TypeTag::IntegerVector, std_.integer, std_.natural);
    S.arrayType("real_vector", TypeTag::None, std_.real, std_.natural);
    S.arrayType("time_vector", TypeTag::None, std_.time, std_.natural);
    S.enumType("file_open_kind", TypeTag::FileOpenKind, {"read_mode", "write_mode", "append_mode"});
    S.enumType("file_open_status", TypeTag::FileOpenStatus, {"open_ok", "status_error", "name_error", "mode_error"});
    for (const Type* t : {std_.integer, std_.real, std_.time}) {
        S.fn("minimum", Builtin::Minimum, {t, t}, t);
        S.fn("maximum", Builtin::Maximum, {t, t}, t);
    }
    S.fn("now", Builtin::Ignored, {}, std_.time);
    for (const Type* t : {std_.integer, std_.real, std_.time, std_.boolean, std_.bit, std_.character, std_.bitVector})
        S.fn("to_string", Builtin::ToString, {t}, std_.string);
    S.fn("to_hstring", Builtin::ToHString, {std_.bitVector}, std_.string);
    S.fn("to_ostring", Builtin::ToOString, {std_.bitVector}, std_.string);
    S.fn("rising_edge", Builtin::RisingEdge, {std_.bit}, std_.boolean, {"s"});
    S.fn("falling_edge", Builtin::FallingEdge, {std_.bit}, std_.boolean, {"s"});

    // ---------------- std.textio / std.env ----------------
    {
        PackageInfo* tio = newPkg("std", "textio");
        Pkg T{*this, tio};
        Type line;
        line.kind = TypeKind::Access;
        line.tag = TypeTag::Line;
        line.name = "line";
        T.typeSym("line", types.make(std::move(line)));
        Type text;
        text.kind = TypeKind::File;
        text.tag = TypeTag::Text;
        text.name = "text";
        T.typeSym("text", types.make(std::move(text)));
        T.enumType("side", TypeTag::None, {"right", "left"});
        T.subtype("width", std_.integer, {0, kIntMax, true});
        for (const char* p : {"readline", "writeline", "read", "write", "hread", "hwrite", "oread", "owrite",
                              "sread", "swrite", "tee", "flush", "deallocate", "file_open", "file_close"}) {
            Symbol* s = T.fn(p, Builtin::Ignored, {}, nullptr);
            s->isFunction = false;
        }
        T.fn("endfile", Builtin::Ignored, {}, std_.boolean);
        PackageInfo* env = newPkg("std", "env");
        Pkg En{*this, env};
        for (const char* p : {"stop", "finish"}) {
            Symbol* s = En.fn(p, Builtin::Ignored, {}, nullptr);
            s->isFunction = false;
        }
        En.fn("resolution_limit", Builtin::Ignored, {}, std_.time);
    }

    // ---------------- ieee.std_logic_1164 ----------------
    PackageInfo* l1164 = newPkg("ieee", "std_logic_1164");
    Pkg L{*this, l1164};
    const Type* sul = L.enumType("std_ulogic", TypeTag::StdUlogic,
                                 {"'U'", "'X'", "'0'", "'1'", "'Z'", "'W'", "'L'", "'H'", "'-'"});
    std_.stdUlogic = sul;
    std_.stdLogic = L.alias("std_logic", sul, true);
    const Type* sulv = L.arrayType("std_ulogic_vector", TypeTag::StdUlogicVector, sul, std_.natural);
    std_.stdUlogicVector = sulv;
    // VHDL-2008: std_logic_vector is a resolved subtype of std_ulogic_vector.
    // Accepting both 1993 and 2008 code this way is what most tools do.
    std_.stdLogicVector = L.alias("std_logic_vector", sulv, true);
    const Type* slv = std_.stdLogicVector;
    L.subtype("x01", sul, {SL_X, SL_1, true});
    L.subtype("x01z", sul, {SL_X, SL_Z, true});
    L.subtype("ux01", sul, {SL_U, SL_1, true});
    L.subtype("ux01z", sul, {SL_U, SL_Z, true});
    {
        const std::pair<const char*, Builtin> logic[] = {{"and", Builtin::LogicAnd}, {"or", Builtin::LogicOr},
                                                         {"nand", Builtin::LogicNand}, {"nor", Builtin::LogicNor},
                                                         {"xor", Builtin::LogicXor}, {"xnor", Builtin::LogicXnor}};
        for (const auto& [n, b] : logic) {
            L.fn(n, b, {sul, sul}, sul);
            L.fn(n, b, {sulv, sulv}, sulv);
            L.fn(n, b, {sul, sulv}, sulv); // VHDL-2008 scalar-array forms
            L.fn(n, b, {sulv, sul}, sulv);
        }
        L.fn("not", Builtin::LogicNot, {sul}, sul);
        L.fn("not", Builtin::LogicNot, {sulv}, sulv);
        const std::pair<const char*, Builtin> red[] = {{"and", Builtin::AndReduce},   {"or", Builtin::OrReduce},
                                                       {"nand", Builtin::NandReduce}, {"nor", Builtin::NorReduce},
                                                       {"xor", Builtin::XorReduce},   {"xnor", Builtin::XnorReduce}};
        for (const auto& [n, b] : red)
            L.fn(n, b, {sulv}, sul); // VHDL-2008 unary reduction
        for (const char* s : {"sll", "srl", "rol", "ror"}) {
            const Builtin b = std::string(s) == "sll"   ? Builtin::NumSll
                              : std::string(s) == "srl" ? Builtin::NumSrl
                              : std::string(s) == "rol" ? Builtin::NumRol
                                                        : Builtin::NumRor;
            L.fn(s, b, {sulv, std_.integer}, sulv);
        }
    }
    L.fn("rising_edge", Builtin::RisingEdge, {sul}, std_.boolean, {"s"});
    L.fn("falling_edge", Builtin::FallingEdge, {sul}, std_.boolean, {"s"});
    L.fn("to_bit", Builtin::ToBit, {sul, std_.bit}, std_.bit, {"s", "xmap"});
    L.fn("to_bitvector", Builtin::ToBitVector, {sulv, std_.bit}, std_.bitVector, {"s", "xmap"});
    L.fn("to_stdulogic", Builtin::ToStdULogic, {std_.bit}, sul, {"b"});
    L.fn("to_stdlogicvector", Builtin::ToStdLogicVector, {std_.bitVector}, slv, {"b"});
    L.fn("to_stdlogicvector", Builtin::ToStdLogicVector, {sulv}, slv, {"s"});
    L.fn("to_stdulogicvector", Builtin::ToStdULogicVector, {std_.bitVector}, sulv, {"b"});
    L.fn("to_stdulogicvector", Builtin::ToStdULogicVector, {slv}, sulv, {"s"});
    L.fn("to_slv", Builtin::ToStdLogicVector, {std_.bitVector}, slv, {"b"});
    L.fn("to_sulv", Builtin::ToStdULogicVector, {std_.bitVector}, sulv, {"b"});
    L.fn("to_bv", Builtin::ToBitVector, {sulv, std_.bit}, std_.bitVector, {"s", "xmap"});
    for (const char* n : {"to_x01", "to_x01z", "to_ux01"}) {
        const Builtin b = std::string(n) == "to_x01" ? Builtin::ToX01 : std::string(n) == "to_x01z" ? Builtin::ToX01Z : Builtin::ToUX01;
        L.fn(n, b, {sul}, sul);
        L.fn(n, b, {sulv}, sulv);
        L.fn(n, b, {std_.bit}, sul);
        L.fn(n, b, {std_.bitVector}, sulv);
    }
    L.fn("is_x", Builtin::IsX, {sul}, std_.boolean);
    L.fn("is_x", Builtin::IsX, {sulv}, std_.boolean);
    L.fn("resolved", Builtin::Resolved, {sulv}, sul);
    L.fn("??", Builtin::Condition, {sul}, std_.boolean);
    for (const auto& [n, b] : {std::pair<const char*, Builtin>{"?=", Builtin::MatchEq}, {"?/=", Builtin::MatchNe},
                               {"?<", Builtin::MatchLt}, {"?<=", Builtin::MatchLe}, {"?>", Builtin::MatchGt},
                               {"?>=", Builtin::MatchGe}}) {
        L.fn(n, b, {sul, sul}, sul);
        if (b == Builtin::MatchEq || b == Builtin::MatchNe)
            L.fn(n, b, {sulv, sulv}, sul);
    }
    L.fn("to_string", Builtin::ToString, {sul}, std_.string);
    L.fn("to_string", Builtin::ToString, {sulv}, std_.string);
    L.fn("to_hstring", Builtin::ToHString, {sulv}, std_.string);
    L.fn("to_ostring", Builtin::ToOString, {sulv}, std_.string);

    // ---------------- numeric packages ----------------
    auto numericOps = [&](Pkg& N, const Type* U, const Type* Sg, const Type* elem) {
        const Type* nat = std_.natural;
        const Type* in = std_.integer;
        const Type* bl = std_.boolean;
        const std::pair<const char*, Builtin> arith[] = {{"+", Builtin::NumAdd}, {"-", Builtin::NumSub},
                                                         {"*", Builtin::NumMul}, {"/", Builtin::NumDiv},
                                                         {"rem", Builtin::NumRem}, {"mod", Builtin::NumMod}};
        for (const auto& [n, b] : arith) {
            N.fn(n, b, {U, U}, U);
            N.fn(n, b, {Sg, Sg}, Sg);
            N.fn(n, b, {U, nat}, U);
            N.fn(n, b, {nat, U}, U);
            N.fn(n, b, {Sg, in}, Sg);
            N.fn(n, b, {in, Sg}, Sg);
            if (b == Builtin::NumAdd || b == Builtin::NumSub) {
                N.fn(n, b, {U, elem}, U); // VHDL-2008
                N.fn(n, b, {elem, U}, U);
                N.fn(n, b, {Sg, elem}, Sg);
                N.fn(n, b, {elem, Sg}, Sg);
            }
        }
        N.fn("-", Builtin::NumNeg, {Sg}, Sg);
        N.fn("abs", Builtin::NumAbs, {Sg}, Sg);
        const std::pair<const char*, Builtin> cmp[] = {{"=", Builtin::NumEq},  {"/=", Builtin::NumNe},
                                                       {"<", Builtin::NumLt},  {"<=", Builtin::NumLe},
                                                       {">", Builtin::NumGt},  {">=", Builtin::NumGe}};
        for (const auto& [n, b] : cmp) {
            N.fn(n, b, {U, U}, bl);
            N.fn(n, b, {Sg, Sg}, bl);
            N.fn(n, b, {U, nat}, bl);
            N.fn(n, b, {nat, U}, bl);
            N.fn(n, b, {Sg, in}, bl);
            N.fn(n, b, {in, Sg}, bl);
        }
        for (const auto& [n, b] : {std::pair<const char*, Builtin>{"?=", Builtin::MatchEq}, {"?/=", Builtin::MatchNe},
                                   {"?<", Builtin::MatchLt}, {"?<=", Builtin::MatchLe}, {"?>", Builtin::MatchGt},
                                   {"?>=", Builtin::MatchGe}}) {
            N.fn(n, b, {U, U}, elem);
            N.fn(n, b, {Sg, Sg}, elem);
        }
        for (const auto& [n, b] : {std::pair<const char*, Builtin>{"shift_left", Builtin::ShiftLeft},
                                   {"shift_right", Builtin::ShiftRight}, {"rotate_left", Builtin::RotateLeft},
                                   {"rotate_right", Builtin::RotateRight}}) {
            N.fn(n, b, {U, nat}, U, {"arg", "count"});
            N.fn(n, b, {Sg, nat}, Sg, {"arg", "count"});
        }
        for (const auto& [n, b] : {std::pair<const char*, Builtin>{"sll", Builtin::NumSll}, {"srl", Builtin::NumSrl},
                                   {"rol", Builtin::NumRol}, {"ror", Builtin::NumRor}, {"sla", Builtin::NumSla},
                                   {"sra", Builtin::NumSra}}) {
            N.fn(n, b, {U, in}, U);
            N.fn(n, b, {Sg, in}, Sg);
        }
        N.fn("resize", Builtin::Resize, {Sg, nat}, Sg, {"arg", "new_size"});
        N.fn("resize", Builtin::Resize, {U, nat}, U, {"arg", "new_size"});
        N.fn("resize", Builtin::Resize, {Sg, Sg}, Sg, {"arg", "size_res"});
        N.fn("resize", Builtin::Resize, {U, U}, U, {"arg", "size_res"});
        N.fn("to_integer", Builtin::ToInteger, {U}, nat, {"arg"});
        N.fn("to_integer", Builtin::ToInteger, {Sg}, in, {"arg"});
        N.fn("to_unsigned", Builtin::ToUnsigned, {nat, nat}, U, {"arg", "size"});
        N.fn("to_unsigned", Builtin::ToUnsigned, {nat, U}, U, {"arg", "size_res"});
        N.fn("to_signed", Builtin::ToSigned, {in, nat}, Sg, {"arg", "size"});
        N.fn("to_signed", Builtin::ToSigned, {in, Sg}, Sg, {"arg", "size_res"});
        N.fn("std_match", Builtin::StdMatch, {U, U}, bl);
        N.fn("std_match", Builtin::StdMatch, {Sg, Sg}, bl);
        N.fn("to_01", Builtin::To01, {U, elem}, U, {"s", "xmap"});
        N.fn("to_01", Builtin::To01, {Sg, elem}, Sg, {"s", "xmap"});
        N.fn("find_leftmost", Builtin::FindLeftmost, {U, elem}, in);
        N.fn("find_rightmost", Builtin::FindRightmost, {U, elem}, in);
        N.fn("find_leftmost", Builtin::FindLeftmost, {Sg, elem}, in);
        N.fn("find_rightmost", Builtin::FindRightmost, {Sg, elem}, in);
        N.fn("minimum", Builtin::Minimum, {U, U}, U);
        N.fn("maximum", Builtin::Maximum, {U, U}, U);
        N.fn("minimum", Builtin::Minimum, {Sg, Sg}, Sg);
        N.fn("maximum", Builtin::Maximum, {Sg, Sg}, Sg);
        const std::pair<const char*, Builtin> logic[] = {{"and", Builtin::LogicAnd}, {"or", Builtin::LogicOr},
                                                         {"nand", Builtin::LogicNand}, {"nor", Builtin::LogicNor},
                                                         {"xor", Builtin::LogicXor}, {"xnor", Builtin::LogicXnor}};
        for (const auto& [n, b] : logic) {
            N.fn(n, b, {U, U}, U);
            N.fn(n, b, {Sg, Sg}, Sg);
        }
        N.fn("not", Builtin::LogicNot, {U}, U);
        N.fn("not", Builtin::LogicNot, {Sg}, Sg);
        const std::pair<const char*, Builtin> red[] = {{"and", Builtin::AndReduce}, {"or", Builtin::OrReduce},
                                                       {"xor", Builtin::XorReduce}, {"nand", Builtin::NandReduce},
                                                       {"nor", Builtin::NorReduce}, {"xnor", Builtin::XnorReduce}};
        for (const auto& [n, b] : red) {
            N.fn(n, b, {U}, elem);
            N.fn(n, b, {Sg}, elem);
        }
        N.fn("to_string", Builtin::ToString, {U}, std_.string);
        N.fn("to_hstring", Builtin::ToHString, {U}, std_.string);
    };
    {
        PackageInfo* ns = newPkg("ieee", "numeric_std");
        Pkg N{*this, ns};
        std_.unsignedT = N.arrayType("unsigned", TypeTag::Unsigned, std_.stdLogic, std_.natural);
        std_.signedT = N.arrayType("signed", TypeTag::Signed, std_.stdLogic, std_.natural);
        N.alias("unresolved_unsigned", std_.unsignedT);
        N.alias("unresolved_signed", std_.signedT);
        N.alias("u_unsigned", std_.unsignedT);
        N.alias("u_signed", std_.signedT);
        numericOps(N, std_.unsignedT, std_.signedT, sul);
    }
    {
        PackageInfo* nb = newPkg("ieee", "numeric_bit");
        Pkg N{*this, nb};
        std_.bitUnsigned = N.arrayType("unsigned", TypeTag::Unsigned, std_.bit, std_.natural);
        std_.bitSigned = N.arrayType("signed", TypeTag::Signed, std_.bit, std_.natural);
        numericOps(N, std_.bitUnsigned, std_.bitSigned, std_.bit);
        N.fn("rising_edge", Builtin::RisingEdge, {std_.bit}, std_.boolean, {"s"});
        N.fn("falling_edge", Builtin::FallingEdge, {std_.bit}, std_.boolean, {"s"});
    }
    // numeric_std_unsigned (VHDL-2008) and Synopsys std_logic_unsigned/signed:
    // arithmetic directly on std_logic_vector
    auto slvArith = [&](Pkg& N, bool isSigned, bool synopsys) {
        const Type* in = synopsys ? std_.integer : std_.natural;
        const Builtin conv = synopsys ? Builtin::ConvInteger : Builtin::ToInteger;
        for (const auto& [n, b] : {std::pair<const char*, Builtin>{"+", Builtin::NumAdd}, {"-", Builtin::NumSub},
                                   {"*", Builtin::NumMul}, {"/", Builtin::NumDiv}, {"rem", Builtin::NumRem},
                                   {"mod", Builtin::NumMod}}) {
            if (synopsys && (b == Builtin::NumDiv || b == Builtin::NumRem || b == Builtin::NumMod))
                continue;
            N.fn(n, b, {slv, slv}, slv);
            if (b != Builtin::NumMul || !synopsys) {
                N.fn(n, b, {slv, in}, slv);
                N.fn(n, b, {in, slv}, slv);
            }
            if (synopsys && b != Builtin::NumMul) {
                N.fn(n, b, {slv, std_.stdLogic}, slv);
                N.fn(n, b, {std_.stdLogic, slv}, slv);
            }
        }
        if (isSigned) {
            N.fn("-", Builtin::NumNeg, {slv}, slv);
            N.fn("abs", Builtin::NumAbs, {slv}, slv);
        }
        N.fn("+", Builtin::None, {slv}, slv); // unary plus: identity
        for (const auto& [n, b] : {std::pair<const char*, Builtin>{"=", Builtin::NumEq}, {"/=", Builtin::NumNe},
                                   {"<", Builtin::NumLt}, {"<=", Builtin::NumLe}, {">", Builtin::NumGt},
                                   {">=", Builtin::NumGe}}) {
            if (!synopsys || (b != Builtin::NumEq && b != Builtin::NumNe))
                N.fn(n, b, {slv, slv}, std_.boolean);
            N.fn(n, b, {slv, in}, std_.boolean);
            N.fn(n, b, {in, slv}, std_.boolean);
        }
        if (synopsys) {
            N.fn("conv_integer", conv, {slv}, std_.integer);
            N.fn("shl", Builtin::Shl, {slv, slv}, slv);
            N.fn("shr", Builtin::Shr, {slv, slv}, slv);
        }
        else {
            N.fn("to_integer", conv, {slv}, std_.natural, {"arg"});
            N.fn("to_stdlogicvector", Builtin::ToUnsigned, {std_.natural, std_.natural}, slv, {"arg", "size"});
            N.fn("to_slv", Builtin::ToUnsigned, {std_.natural, std_.natural}, slv, {"arg", "size"});
            N.fn("to_stdulogicvector", Builtin::ToUnsigned, {std_.natural, std_.natural}, sulv, {"arg", "size"});
            N.fn("to_sulv", Builtin::ToUnsigned, {std_.natural, std_.natural}, sulv, {"arg", "size"});
            N.fn("shift_left", Builtin::ShiftLeft, {slv, std_.natural}, slv, {"arg", "count"});
            N.fn("shift_right", Builtin::ShiftRight, {slv, std_.natural}, slv, {"arg", "count"});
            N.fn("rotate_left", Builtin::RotateLeft, {slv, std_.natural}, slv, {"arg", "count"});
            N.fn("rotate_right", Builtin::RotateRight, {slv, std_.natural}, slv, {"arg", "count"});
            N.fn("resize", Builtin::Resize, {slv, std_.natural}, slv, {"arg", "new_size"});
            N.fn("maximum", Builtin::Maximum, {slv, slv}, slv);
            N.fn("minimum", Builtin::Minimum, {slv, slv}, slv);
        }
    };
    {
        PackageInfo* p = newPkg("ieee", "numeric_std_unsigned");
        Pkg N{*this, p};
        slvArith(N, false, false);
        PackageInfo* su = newPkg("ieee", "std_logic_unsigned");
        Pkg U{*this, su};
        slvArith(U, false, true);
        PackageInfo* ss = newPkg("ieee", "std_logic_signed");
        Pkg Sg{*this, ss};
        slvArith(Sg, true, true);
    }
    {
        // Synopsys std_logic_arith: its own UNSIGNED / SIGNED types
        PackageInfo* pa = newPkg("ieee", "std_logic_arith");
        Pkg A{*this, pa};
        const Type* U = A.arrayType("unsigned", TypeTag::ArithUnsigned, std_.stdLogic, std_.natural);
        const Type* Sg = A.arrayType("signed", TypeTag::ArithSigned, std_.stdLogic, std_.natural);
        std_.arithUnsigned = U;
        std_.arithSigned = Sg;
        A.subtype("small_int", std_.integer, {0, 1, true});
        const Type* in = std_.integer;
        const Type* sl = std_.stdLogic;
        for (const auto& [n, b] : {std::pair<const char*, Builtin>{"+", Builtin::NumAdd}, {"-", Builtin::NumSub}}) {
            for (const Type* r : {U, Sg}) {
                A.fn(n, b, {r, r}, r);
                A.fn(n, b, {r, in}, r);
                A.fn(n, b, {in, r}, r);
                A.fn(n, b, {r, sl}, r);
                A.fn(n, b, {sl, r}, r);
                A.fn(n, b, {r, r}, slv);
                A.fn(n, b, {r, in}, slv);
                A.fn(n, b, {in, r}, slv);
            }
            A.fn(n, b, {U, Sg}, Sg);
            A.fn(n, b, {Sg, U}, Sg);
        }
        A.fn("*", Builtin::NumMul, {U, U}, U);
        A.fn("*", Builtin::NumMul, {Sg, Sg}, Sg);
        A.fn("*", Builtin::NumMul, {U, U}, slv);
        A.fn("*", Builtin::NumMul, {Sg, Sg}, slv);
        A.fn("-", Builtin::NumNeg, {Sg}, Sg);
        A.fn("abs", Builtin::NumAbs, {Sg}, Sg);
        for (const auto& [n, b] : {std::pair<const char*, Builtin>{"=", Builtin::NumEq}, {"/=", Builtin::NumNe},
                                   {"<", Builtin::NumLt}, {"<=", Builtin::NumLe}, {">", Builtin::NumGt},
                                   {">=", Builtin::NumGe}}) {
            for (const Type* r : {U, Sg}) {
                A.fn(n, b, {r, r}, std_.boolean);
                A.fn(n, b, {r, in}, std_.boolean);
                A.fn(n, b, {in, r}, std_.boolean);
            }
            A.fn(n, b, {U, Sg}, std_.boolean);
            A.fn(n, b, {Sg, U}, std_.boolean);
        }
        for (const Type* t : {U, Sg, in}) {
            A.fn("conv_integer", Builtin::ConvInteger, {t}, in);
            A.fn("conv_unsigned", Builtin::ConvUnsigned, {t, in}, U);
            A.fn("conv_signed", Builtin::ConvSigned, {t, in}, Sg);
            A.fn("conv_std_logic_vector", Builtin::ConvStdLogicVector, {t, in}, slv);
        }
        A.fn("conv_integer", Builtin::ConvInteger, {sl}, std_.integer);
        A.fn("conv_unsigned", Builtin::ConvUnsigned, {sl, in}, U);
        A.fn("conv_std_logic_vector", Builtin::ConvStdLogicVector, {sl, in}, slv);
        A.fn("ext", Builtin::Ext, {slv, in}, slv);
        A.fn("sxt", Builtin::Sxt, {slv, in}, slv);
        A.fn("shl", Builtin::Shl, {U, U}, U);
        A.fn("shl", Builtin::Shl, {Sg, U}, Sg);
        A.fn("shr", Builtin::Shr, {U, U}, U);
        A.fn("shr", Builtin::Shr, {Sg, U}, Sg);

        PackageInfo* misc = newPkg("ieee", "std_logic_misc");
        Pkg M{*this, misc};
        for (const auto& [n, b] : {std::pair<const char*, Builtin>{"and_reduce", Builtin::AndReduce},
                                   {"or_reduce", Builtin::OrReduce}, {"xor_reduce", Builtin::XorReduce},
                                   {"nand_reduce", Builtin::NandReduce}, {"nor_reduce", Builtin::NorReduce},
                                   {"xnor_reduce", Builtin::XnorReduce}}) {
            M.fn(n, b, {slv}, sl);
            M.fn(n, b, {sulv}, sul);
        }
    }

    // ---------------- ieee.math_real ----------------
    {
        PackageInfo* mr = newPkg("ieee", "math_real");
        Pkg M{*this, mr};
        const Type* r = std_.real;
        M.constant("math_e", r, Value::real(std::numbers::e));
        M.constant("math_pi", r, Value::real(std::numbers::pi));
        M.constant("math_2_pi", r, Value::real(2 * std::numbers::pi));
        M.constant("math_pi_over_2", r, Value::real(std::numbers::pi / 2));
        M.constant("math_log_of_2", r, Value::real(std::numbers::ln2));
        M.constant("math_log_of_10", r, Value::real(std::numbers::ln10));
        M.constant("math_log2_of_e", r, Value::real(std::numbers::log2e));
        M.constant("math_log10_of_e", r, Value::real(std::numbers::log10e));
        M.constant("math_sqrt_2", r, Value::real(std::numbers::sqrt2));
        M.constant("math_1_over_sqrt_2", r, Value::real(1 / std::numbers::sqrt2));
        M.constant("math_1_over_pi", r, Value::real(1 / std::numbers::pi));
        M.constant("math_deg_to_rad", r, Value::real(std::numbers::pi / 180));
        M.constant("math_rad_to_deg", r, Value::real(180 / std::numbers::pi));
        const std::pair<const char*, Builtin> f1[] = {
            {"ceil", Builtin::MathCeil},   {"floor", Builtin::MathFloor}, {"round", Builtin::MathRound},
            {"trunc", Builtin::MathTrunc}, {"log2", Builtin::MathLog2},   {"log10", Builtin::MathLog10},
            {"log", Builtin::MathLog},     {"exp", Builtin::MathExp},     {"sqrt", Builtin::MathSqrt},
            {"sign", Builtin::MathSign},   {"sin", Builtin::MathSin},     {"cos", Builtin::MathCos}};
        for (const auto& [n, b] : f1)
            M.fn(n, b, {r}, r, {"x"});
        M.fn("log", Builtin::MathLog, {r, r}, r, {"x", "base"});
        M.fn("realmax", Builtin::MathRealMax, {r, r}, r, {"x", "y"});
        M.fn("realmin", Builtin::MathRealMin, {r, r}, r, {"x", "y"});
        M.fn("mod", Builtin::MathRealMod, {r, r}, r, {"x", "y"});
        M.fn("**", Builtin::MathRealPow, {r, r}, r, {"x", "y"});
        M.fn("**", Builtin::MathRealPow, {std_.integer, r}, r, {"x", "y"});
        M.fn("**", Builtin::MathRealPow, {r, std_.integer}, r, {"x", "y"});
        Symbol* u = M.fn("uniform", Builtin::Ignored, {}, nullptr);
        u->isFunction = false;
    }
    {
        PackageInfo* t = newPkg("ieee", "std_logic_textio");
        Pkg T{*this, t};
        for (const char* p : {"read", "write", "hread", "hwrite", "oread", "owrite"}) {
            Symbol* s = T.fn(p, Builtin::Ignored, {}, nullptr);
            s->isFunction = false;
        }
    }
}

// ===========================================================================
// Semantics
// ===========================================================================


SVal Elaborator::callBuiltin(Builtin b, const Symbol& sym, std::vector<SVal>& args, const Type* result, Ctx& ctx,
                             Loc l) {
    auto vecOf = [&](const Type* base, std::uint32_t w) {
        return types.constrainedArray(base, {IntRange{static_cast<std::int64_t>(w) - 1, 0, false}});
    };
    auto widthOf = [&](const SVal& v) -> std::uint32_t { return v.type ? bitWidth(*v.type).value_or(0) : 0; };
    auto isSignedVec = [](const Type* t) {
        const auto tag = t->baseTag();
        return tag == TypeTag::Signed || tag == TypeTag::ArithSigned;
    };
    auto knownInt = [](const SVal& v) -> std::optional<std::int64_t> {
        if (v.known() && v.value->kind == Value::Kind::Int)
            return v.value->i;
        return std::nullopt;
    };
    auto knownReal = [](const SVal& v) -> std::optional<double> {
        if (!v.known())
            return std::nullopt;
        if (v.value->kind == Value::Kind::Real)
            return v.value->r;
        if (v.value->kind == Value::Kind::Int)
            return static_cast<double>(v.value->i);
        return std::nullopt;
    };
    auto intBits = [&](const SVal& v, std::uint32_t w) {
        return bitsOf(v, l).resized(w, isSignedInteger(*v.type));
    };
    auto boolean = [&](const ir::SigSpec& bit) { return makeBits(std_.boolean, bit); };
    // Signed interpretation of std_logic_vector in ieee.std_logic_signed
    const bool slvSigned = sym.library == "ieee.std_logic_signed";
    auto vecSigned = [&](const SVal& v) { return isSignedVec(v.type) || (slvSigned && v.type->isArray()); };

    switch (b) {
        case Builtin::None:
            if (!args.empty())
                return convert(args[0], result, ctx, l);
            break;
        case Builtin::Ignored: return SVal{result ? result : std_.boolean, std::nullopt, {}};

        // ---------------- std_logic_1164 ----------------
        case Builtin::RisingEdge:
        case Builtin::FallingEdge:
            // Edges are recognized structurally by the process lowering; an
            // edge used anywhere else is not modelled.
            unsupported("IR-UNSUPPORTED-EDGE",
                        std::string(b == Builtin::RisingEdge ? "rising_edge" : "falling_edge") +
                            " outside a clocked process condition",
                        l);
            return opaque(std_.boolean, {bitsOf(args[0], l)}, "edge", l);
        case Builtin::ToBit:
        case Builtin::ToStdULogic:
        case Builtin::ToX01:
        case Builtin::ToX01Z:
        case Builtin::ToUX01:
        case Builtin::To01: {
            const Type* rt = result;
            if (args[0].type->isArray())
                rt = types.constrainedArray(&result->baseType(), {*args[0].type->indexRanges.front()});
            return makeBits(rt, bitsOf(args[0], l));
        }
        case Builtin::ToBitVector:
        case Builtin::ToStdLogicVector:
        case Builtin::ToStdULogicVector: {
            const auto r = args[0].type->indexRanges.empty() ? std::nullopt : args[0].type->indexRanges.front();
            const Type* rt = types.constrainedArray(&result->baseType(), {r.value_or(IntRange{0, -1, true})});
            if (b == Builtin::ToStdLogicVector && result != &result->baseType())
                rt = types.constrainedArray(result, {r.value_or(IntRange{0, -1, true})});
            return makeBits(rt, bitsOf(args[0], l));
        }
        case Builtin::IsX: return makeConst(std_.boolean, Value::integer(0));
        case Builtin::Resolved: return makeBits(std_.stdUlogic, bitsOf(args[0], l).extract(0, 1));
        case Builtin::Condition: return makeBits(std_.boolean, bitsOf(args[0], l));
        case Builtin::LogicAnd:
        case Builtin::LogicOr:
        case Builtin::LogicNand:
        case Builtin::LogicNor:
        case Builtin::LogicXor:
        case Builtin::LogicXnor: {
            ir::SigSpec a = bitsOf(args[0], l);
            ir::SigSpec c = bitsOf(args[1], l);
            const Type* rt = args[0].type->isArray() ? args[0].type : args[1].type;
            const std::uint32_t w = std::max(a.width(), c.width());
            if (a.width() == 1 && w > 1)
                a = a.repeated(w);
            if (c.width() == 1 && w > 1)
                c = c.repeated(w);
            if (a.width() != c.width()) {
                error("VHDL-LENGTH", "operands of '" + sym.display + "' have different lengths (" +
                                         std::to_string(a.width()) + " and " + std::to_string(c.width()) + ")",
                      l);
                return makeBits(rt, ir::SigSpec::allX(w));
            }
            ir::CellKind k = b == Builtin::LogicAnd || b == Builtin::LogicNand   ? ir::CellKind::And
                             : b == Builtin::LogicOr || b == Builtin::LogicNor   ? ir::CellKind::Or
                             : b == Builtin::LogicXor                            ? ir::CellKind::Xor
                                                                                 : ir::CellKind::Xnor;
            ir::SigSpec y = cell(k, {a, c}, w, l);
            if (b == Builtin::LogicNand || b == Builtin::LogicNor)
                y = cell(ir::CellKind::Not, {y}, w, l);
            return makeBits(rt, y);
        }
        case Builtin::LogicNot: {
            ir::SigSpec a = bitsOf(args[0], l);
            return makeBits(args[0].type, cell(ir::CellKind::Not, {a}, a.width(), l));
        }
        case Builtin::AndReduce:
        case Builtin::OrReduce:
        case Builtin::XorReduce:
        case Builtin::NandReduce:
        case Builtin::NorReduce:
        case Builtin::XnorReduce: {
            ir::SigSpec a = bitsOf(args[0], l);
            const ir::CellKind k = b == Builtin::AndReduce || b == Builtin::NandReduce ? ir::CellKind::ReduceAnd
                                   : b == Builtin::OrReduce || b == Builtin::NorReduce ? ir::CellKind::ReduceOr
                                                                                       : ir::CellKind::ReduceXor;
            ir::SigSpec y = cell(k, {a}, 1, l);
            if (b == Builtin::NandReduce || b == Builtin::NorReduce || b == Builtin::XnorReduce)
                y = cell(ir::CellKind::Not, {y}, 1, l);
            return makeBits(result, y);
        }
        case Builtin::MatchEq:
        case Builtin::MatchNe:
        case Builtin::MatchLt:
        case Builtin::MatchLe:
        case Builtin::MatchGt:
        case Builtin::MatchGe: {
            ir::SigSpec a = bitsOf(args[0], l);
            ir::SigSpec c = bitsOf(args[1], l);
            const std::uint32_t w = std::max(a.width(), c.width());
            const bool sg = isSignedVec(args[0].type);
            const ir::CellKind k = b == Builtin::MatchEq   ? ir::CellKind::Eq
                                   : b == Builtin::MatchNe ? ir::CellKind::Ne
                                   : b == Builtin::MatchLt ? ir::CellKind::Lt
                                   : b == Builtin::MatchLe ? ir::CellKind::Le
                                   : b == Builtin::MatchGt ? ir::CellKind::Gt
                                                           : ir::CellKind::Ge;
            return makeBits(result, cell(k, {a.resized(w, sg), c.resized(w, sg)}, 1, l, sg));
        }
        case Builtin::StdMatch: {
            ir::SigSpec a = bitsOf(args[0], l);
            ir::SigSpec c = bitsOf(args[1], l);
            return boolean(cell(ir::CellKind::EqWild, {a, c}, 1, l));
        }

        // ---------------- numeric ----------------
        case Builtin::ToInteger:
        case Builtin::ConvInteger: {
            const SVal& a = args[0];
            if (!a.type->isArray()) {
                if (a.type->isIntegerLike())
                    return convert(a, result, ctx, l);
                return makeBits(result, bitsOf(a, l).resized(32, false)); // std_logic -> 0/1
            }
            const bool sg = vecSigned(a) || (b == Builtin::ConvInteger && slvSigned);
            ir::SigSpec v = bitsOf(a, l);
            if (v.width() > 32 && !reported_.contains("toint32"))
                warn("VHDL-INTEGER-WIDTH", "conversion of a vector wider than 32 bits to integer keeps 32 bits", l);
            const Type* rt = sg ? std_.integer : (result ? result : std_.natural);
            return makeBits(rt->kind == TypeKind::Integer && rt->constrained && !sg ? std_.integer : rt,
                            v.resized(32, sg));
        }
        case Builtin::ToUnsigned:
        case Builtin::ToSigned:
        case Builtin::ConvUnsigned:
        case Builtin::ConvSigned:
        case Builtin::ConvStdLogicVector: {
            std::optional<std::int64_t> size;
            if (args[1].type->isArray())
                size = static_cast<std::int64_t>(widthOf(args[1])); // size_res form
            else
                size = knownInt(args[1]);
            if (!size || *size < 0) {
                error("VHDL-NOT-STATIC", "the size argument of '" + sym.display + "' must be static", l);
                return SVal{};
            }
            const auto w = static_cast<std::uint32_t>(*size);
            const Type* base = &result->baseType();
            const Type* rt = vecOf(result == std_.stdLogicVector ? result : base, w);
            const SVal& a = args[0];
            if (a.type->isArray())
                return makeBits(rt, bitsOf(a, l).resized(w, vecSigned(a)));
            return makeBits(rt, intBits(a, std::max<std::uint32_t>(w, 33)).resized(w, false));
        }
        case Builtin::Resize:
        case Builtin::Ext:
        case Builtin::Sxt: {
            std::optional<std::int64_t> size =
                args[1].type->isArray() ? std::optional<std::int64_t>(widthOf(args[1])) : knownInt(args[1]);
            if (!size || *size < 0) {
                error("VHDL-NOT-STATIC", "the size argument of '" + sym.display + "' must be static", l);
                return SVal{};
            }
            const auto w = static_cast<std::uint32_t>(*size);
            ir::SigSpec v = bitsOf(args[0], l);
            const bool sg = b == Builtin::Sxt || (b == Builtin::Resize && isSignedVec(args[0].type));
            ir::SigSpec r;
            if (sg && b == Builtin::Resize && w < v.width() && w > 0) {
                // numeric_std resize(signed): keep the sign bit and the low bits
                r = v.extract(0, w - 1);
                r.append(v.extract(v.width() - 1, 1));
            }
            else {
                r = v.resized(w, sg);
            }
            const Type* rt = vecOf(args[0].type->isArray() ? &args[0].type->baseType() : result, w);
            if (args[0].type->isArray() && args[0].type->baseTag() == TypeTag::StdUlogicVector &&
                args[0].type != &args[0].type->baseType())
                rt = vecOf(args[0].type->base == args[0].type ? args[0].type : std_.stdLogicVector, w);
            return makeBits(rt, r);
        }
        case Builtin::ShiftLeft:
        case Builtin::ShiftRight:
        case Builtin::NumSll:
        case Builtin::NumSrl:
        case Builtin::NumSla:
        case Builtin::NumSra:
        case Builtin::Shl:
        case Builtin::Shr:
        case Builtin::RotateLeft:
        case Builtin::RotateRight:
        case Builtin::NumRol:
        case Builtin::NumRor: {
            const SVal& a = args[0];
            ir::SigSpec v = bitsOf(a, l);
            const std::uint32_t w = v.width();
            const bool sg = vecSigned(a);
            bool left = b == Builtin::ShiftLeft || b == Builtin::NumSll || b == Builtin::NumSla || b == Builtin::Shl ||
                        b == Builtin::RotateLeft || b == Builtin::NumRol;
            const bool rotate = b == Builtin::RotateLeft || b == Builtin::RotateRight || b == Builtin::NumRol ||
                                b == Builtin::NumRor;
            const bool arith = (b == Builtin::ShiftRight && sg) || b == Builtin::NumSra || (b == Builtin::Shr && sg);
            auto count = knownInt(args[1]);
            if (args[1].type->isArray()) {
                // shl/shr(arg, count: unsigned)
                if (auto cv = bitsOf(args[1], l).asConst(); cv && cv->toUint())
                    count = static_cast<std::int64_t>(*cv->toUint());
            }
            if (count) {
                std::int64_t n = *count;
                if (n < 0) { // sll with negative count shifts the other way (VHDL-2008)
                    left = !left;
                    n = -n;
                }
                if (w == 0)
                    return makeBits(a.type, v);
                if (rotate) {
                    const auto k = static_cast<std::uint32_t>(n % w);
                    ir::SigSpec res;
                    if (left) { // the k most significant bits wrap around to the bottom
                        res = v.extract(w - k, k);
                        res.append(v.extract(0, w - k));
                    }
                    else {
                        res = v.extract(k, w - k);
                        res.append(v.extract(0, k));
                    }
                    return makeBits(a.type, res);
                }
                const auto k = static_cast<std::uint32_t>(std::min<std::int64_t>(n, w));
                ir::SigSpec res;
                if (left) {
                    res = ir::SigSpec::constant(0, k);
                    res.append(v.extract(0, w - k));
                }
                else {
                    res = v.extract(k, w - k);
                    res.append(arith ? v.extract(w - 1, 1).repeated(k) : ir::SigSpec::constant(0, k));
                }
                return makeBits(a.type, res);
            }
            ir::SigSpec cnt = args[1].type->isArray() ? bitsOf(args[1], l) : intBits(args[1], 32);
            if (rotate)
                return opaque(a.type, {v, cnt}, "rotate by a variable amount", l);
            const ir::CellKind k = left ? ir::CellKind::Shl : arith ? ir::CellKind::Sshr : ir::CellKind::Shr;
            return makeBits(a.type, cell(k, {v, cnt}, w, l, arith));
        }
        case Builtin::NumAdd:
        case Builtin::NumSub:
        case Builtin::NumMul:
        case Builtin::NumDiv:
        case Builtin::NumMod:
        case Builtin::NumRem: {
            SVal a = args[0];
            SVal c = args[1];
            const bool aVec = a.type->isArray();
            const bool cVec = c.type->isArray();
            const bool sg = (aVec && vecSigned(a)) || (cVec && vecSigned(c));
            std::uint32_t wa = aVec ? widthOf(a) : 0;
            std::uint32_t wc = cVec ? widthOf(c) : 0;
            // scalar operands take the vector's width (std_logic: 1 bit zero-extended)
            ir::SigSpec av;
            ir::SigSpec cv;
            if (!aVec)
                wa = a.type->isIntegerLike() ? wc : 1;
            if (!cVec)
                wc = c.type->isIntegerLike() ? wa : 1;
            std::uint32_t w = 0;
            switch (b) {
                case Builtin::NumMul: w = (aVec && cVec) ? wa + wc : 2 * std::max(wa, wc); break;
                case Builtin::NumDiv: w = aVec ? wa : wc; break;
                case Builtin::NumMod:
                case Builtin::NumRem: w = cVec ? wc : wa; break;
                default: w = std::max(wa, wc); break;
            }
            auto operand = [&](const SVal& v, bool isVec, std::uint32_t ow) {
                if (isVec)
                    return bitsOf(v, l).resized(w, sg);
                if (v.type->isIntegerLike())
                    return intBits(v, std::max<std::uint32_t>(ow, 33)).resized(w, sg);
                return bitsOf(v, l).resized(w, false); // std_logic operand
            };
            av = operand(a, aVec, wa);
            cv = operand(c, cVec, wc);
            const ir::CellKind k = b == Builtin::NumAdd   ? ir::CellKind::Add
                                   : b == Builtin::NumSub ? ir::CellKind::Sub
                                   : b == Builtin::NumMul ? ir::CellKind::Mul
                                   : b == Builtin::NumDiv ? ir::CellKind::Div
                                                          : ir::CellKind::Mod;
            if (b == Builtin::NumMul && aVec && cVec) {
                av = bitsOf(a, l).resized(w, sg);
                cv = bitsOf(c, l).resized(w, sg);
            }
            const Type* base = aVec ? &a.type->baseType() : &c.type->baseType();
            const Type* rt = vecOf(result && result->isArray() && result->baseTag() != base->baseTag()
                                       ? &result->baseType()
                                       : base,
                                   w);
            if (result == std_.stdLogicVector || (result && result->isArray() && result->resolved))
                rt = vecOf(result, w);
            return makeBits(rt, cell(k, {av, cv}, w, l, sg));
        }
        case Builtin::NumNeg:
        case Builtin::NumAbs: {
            ir::SigSpec v = bitsOf(args[0], l);
            if (b == Builtin::NumNeg)
                return makeBits(args[0].type, cell(ir::CellKind::Neg, {v}, v.width(), l, true));
            ir::SigSpec neg = cell(ir::CellKind::Neg, {v}, v.width(), l, true);
            ir::SigSpec sign = v.extract(v.width() - 1, 1);
            return makeBits(args[0].type, mux(sign, v, neg, l));
        }
        case Builtin::NumEq:
        case Builtin::NumNe:
        case Builtin::NumLt:
        case Builtin::NumLe:
        case Builtin::NumGt:
        case Builtin::NumGe: {
            const SVal& a = args[0];
            const SVal& c = args[1];
            const bool aVec = a.type->isArray();
            const bool cVec = c.type->isArray();
            const bool sg = (aVec && vecSigned(a)) || (cVec && vecSigned(c));
            std::uint32_t w = std::max(aVec ? widthOf(a) : 32U, cVec ? widthOf(c) : 32U) + 1;
            auto operand = [&](const SVal& v, bool isVec) {
                if (isVec)
                    return bitsOf(v, l).resized(w, sg);
                return intBits(v, w);
            };
            const ir::CellKind k = b == Builtin::NumEq   ? ir::CellKind::Eq
                                   : b == Builtin::NumNe ? ir::CellKind::Ne
                                   : b == Builtin::NumLt ? ir::CellKind::Lt
                                   : b == Builtin::NumLe ? ir::CellKind::Le
                                   : b == Builtin::NumGt ? ir::CellKind::Gt
                                                         : ir::CellKind::Ge;
            return boolean(cell(k, {operand(a, aVec), operand(c, cVec)}, 1, l, true));
        }
        case Builtin::FindLeftmost:
        case Builtin::FindRightmost:
            return opaque(std_.integer, {bitsOf(args[0], l), bitsOf(args[1], l)}, sym.display, l);

        // ---------------- std.standard (2008) ----------------
        case Builtin::Minimum:
        case Builtin::Maximum: {
            const SVal& a = args[0];
            const SVal& c = args[1];
            if (a.known() && c.known() && a.value->kind == c.value->kind && a.value->kind != Value::Kind::Composite) {
                const bool lt = a.value->kind == Value::Kind::Real ? a.value->r < c.value->r : a.value->i < c.value->i;
                return (b == Builtin::Minimum) == lt ? a : c;
            }
            const bool sg = a.type->isArray() ? isSignedVec(a.type) : isSignedInteger(*a.type);
            ir::SigSpec av = bitsOf(a, l);
            ir::SigSpec cv = bitsOf(c, l);
            const std::uint32_t w = std::max(av.width(), cv.width());
            ir::SigSpec lt = cell(ir::CellKind::Lt, {av.resized(w, sg), cv.resized(w, sg)}, 1, l, sg);
            return makeBits(a.type, b == Builtin::Minimum ? mux(lt, cv, av, l) : mux(lt, av, cv, l));
        }
        case Builtin::ToString:
        case Builtin::ToHString:
        case Builtin::ToOString: return SVal{std_.string, std::nullopt, {}};

        // ---------------- math_real ----------------
        default: break;
    }

    // math_real: constants only
    std::vector<double> xs;
    for (const auto& a : args) {
        auto r = knownReal(a);
        if (!r) {
            error("VHDL-NOT-STATIC", "'" + sym.display + "' (ieee.math_real) can only be evaluated on constants", l);
            return SVal{};
        }
        xs.push_back(*r);
    }
    double r = 0;
    switch (b) {
        case Builtin::MathCeil: r = std::ceil(xs[0]); break;
        case Builtin::MathFloor: r = std::floor(xs[0]); break;
        case Builtin::MathRound: r = std::round(xs[0]); break;
        case Builtin::MathTrunc: r = std::trunc(xs[0]); break;
        case Builtin::MathLog2: r = std::log2(xs[0]); break;
        case Builtin::MathLog10: r = std::log10(xs[0]); break;
        case Builtin::MathLog: r = xs.size() == 2 ? std::log(xs[0]) / std::log(xs[1]) : std::log(xs[0]); break;
        case Builtin::MathExp: r = std::exp(xs[0]); break;
        case Builtin::MathSqrt: r = std::sqrt(xs[0]); break;
        case Builtin::MathSign: r = xs[0] > 0 ? 1.0 : xs[0] < 0 ? -1.0 : 0.0; break;
        case Builtin::MathSin: r = std::sin(xs[0]); break;
        case Builtin::MathCos: r = std::cos(xs[0]); break;
        case Builtin::MathRealPow: r = std::pow(xs[0], xs[1]); break;
        case Builtin::MathRealMax: r = std::max(xs[0], xs[1]); break;
        case Builtin::MathRealMin: r = std::min(xs[0], xs[1]); break;
        case Builtin::MathRealAbs: r = std::fabs(xs[0]); break;
        case Builtin::MathRealMod: r = xs[0] - xs[1] * std::floor(xs[0] / xs[1]); break;
        default:
            unsupported("IR-UNSUPPORTED-FUNCTION", "built-in function '" + sym.display + "'", l);
            return SVal{};
    }
    return makeConst(std_.real, Value::real(r));
}

} // namespace opencdc::vhdl
