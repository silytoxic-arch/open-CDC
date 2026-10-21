#pragma once
// VHDL semantic analysis and elaboration into the Design IR (private to the
// VHDL frontend).
//
// One engine does both constant evaluation and hardware lowering: every
// expression evaluates to an SVal that carries a compile-time Value when
// all its inputs are known (generics, constants, loop indices, literals)
// and IR bits otherwise. Generic-dependent declarations (widths, generate
// ranges, constant tables, user functions such as log2ceil) are therefore
// evaluated with the same code that lowers process logic.
//
// Analysis is lazy: design units are looked up in the library index on
// first use, so files may be listed in any order.

#include "core/Diagnostic.h"
#include "core/Result.h"
#include "frontend/Frontend.h"
#include "frontend/vhdl/VhdlAst.h"
#include "frontend/vhdl/VhdlTypes.h"
#include "ir/BodyEditor.h"
#include "ir/Design.h"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <numeric>
#include <set>
#include <tuple>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace opencdc::vhdl {

// ---------------------------------------------------------------------------
// Symbols and scopes
// ---------------------------------------------------------------------------

struct Scope;
struct PackageInfo;
class Elaborator;

/// Symbolic value: a compile-time Value and/or hardware bits.
struct SVal {
    const Type* type = nullptr;
    std::optional<Value> value;
    ir::SigSpec bits;
    [[nodiscard]] bool known() const noexcept { return value.has_value(); }
    [[nodiscard]] bool valid() const noexcept { return type != nullptr; }
};

enum class SymKind : std::uint8_t {
    Type,
    Constant,      ///< constants, generics, loop indices, function parameters bound to values
    Signal,        ///< signals and ports
    Variable,
    EnumLiteral,
    Subprogram,
    Alias,
    Component,
    Package,
    Library,
    AttributeDecl,
    Unit,          ///< physical unit
    File,
};

/// Native semantics of predefined/IEEE subprograms.
enum class Builtin : std::uint16_t {
    None,
    // std_logic_1164
    RisingEdge, FallingEdge, ToBit, ToBitVector, ToStdULogic, ToStdLogicVector, ToStdULogicVector,
    ToX01, ToX01Z, ToUX01, IsX, Resolved,
    LogicAnd, LogicOr, LogicNand, LogicNor, LogicXor, LogicXnor, LogicNot,     ///< std_ulogic(_vector) operators
    // numeric_std / numeric_bit / std_logic_arith / std_logic_unsigned / numeric_std_unsigned
    ToInteger, ToUnsigned, ToSigned, Resize, ShiftLeft, ShiftRight, RotateLeft, RotateRight, StdMatch, To01,
    NumAdd, NumSub, NumMul, NumDiv, NumMod, NumRem, NumNeg, NumAbs,
    NumEq, NumNe, NumLt, NumLe, NumGt, NumGe,
    NumSll, NumSrl, NumRol, NumRor, NumSla, NumSra,
    ConvInteger, ConvUnsigned, ConvSigned, ConvStdLogicVector, Ext, Sxt, Shl, Shr,
    FindLeftmost, FindRightmost,
    // matching operators (VHDL-2008)
    MatchEq, MatchNe, MatchLt, MatchLe, MatchGt, MatchGe, Condition,
    // reduction functions (std_logic_misc / VHDL-2008 operators)
    AndReduce, OrReduce, XorReduce, NandReduce, NorReduce, XnorReduce,
    // std.standard (VHDL-2008)
    Minimum, Maximum, ToString, ToHString, ToOString,
    // math_real (constant evaluation only)
    MathCeil, MathFloor, MathRound, MathTrunc, MathLog2, MathLog10, MathLog, MathExp, MathSqrt, MathRealPow,
    MathSign, MathRealMin, MathRealMax, MathRealAbs, MathRealMod, MathSin, MathCos,
    // textio and simulation-only routines: accepted, no hardware
    Ignored,
};

struct ParamInfo {
    std::string key;
    std::string display;
    InterfaceDecl::Class cls = InterfaceDecl::Class::Constant;
    Mode mode = Mode::In;
    const Type* type = nullptr;
    const Expr* defaultExpr = nullptr;
    Scope* defaultScope = nullptr;
};

struct Symbol {
    SymKind kind = SymKind::Constant;
    std::string key;
    std::string display;
    Loc loc;
    const Type* type = nullptr;      ///< object/literal type, type of a type symbol, function return type

    SVal value;                      ///< Constant (value may be unknown inside inlined subprograms)
    bool deferred = false;           ///< deferred constant waiting for its package-body value
    int object = -1;                 ///< Signal / Variable: object index in the current body or frame
    std::int64_t pos = 0;            ///< EnumLiteral position; Unit multiplier

    // subprograms
    bool isFunction = false;
    bool pure = true;
    const Subprogram* decl = nullptr;
    Scope* declScope = nullptr;      ///< where the subprogram is declared
    const Subprogram* body = nullptr;
    Scope* bodyScope = nullptr;      ///< scope in which the body is declared
    std::vector<ParamInfo> params;
    bool paramsResolved = false;
    Builtin builtin = Builtin::None;
    bool implicitOp = false;         ///< operator implicitly declared with a type (not used for lookup)

    // alias
    const Expr* aliasTarget = nullptr;
    Scope* aliasScope = nullptr;
    const SubtypeIndication* aliasSubtype = nullptr;

    // component / package / library
    const Decl* component = nullptr;
    PackageInfo* package = nullptr;
    std::string library;

    // user-defined attributes specified for this named entity
    std::vector<std::pair<std::string, SVal>> attributes;
};

struct Scope {
    Scope* parent = nullptr;
    std::unordered_map<std::string, std::vector<Symbol*>> names;
    std::vector<std::string> order;            ///< declaration order of keys (deterministic iteration)
    std::vector<Scope*> usedAll;               ///< "use pkg.all"
    std::vector<std::pair<Scope*, std::string>> usedItems; ///< "use pkg.item"
    std::vector<std::string> libraries;        ///< library clauses visible here
    std::string workLibrary = "work";          ///< library that "work" denotes
    std::string label;                         ///< for messages

    void add(Symbol* s);
    [[nodiscard]] const std::vector<Symbol*>* local(const std::string& key) const;
};

struct PackageInfo {
    std::string library;
    std::string name;
    const DesignUnit* decl = nullptr;
    const DesignUnit* body = nullptr;
    Scope scope;
    Scope bodyScope;
    bool builtin = false;
    bool elaborated = false;
    bool inProgress = false;
    bool bodyElaborated = false;
};

// ---------------------------------------------------------------------------
// Library index
// ---------------------------------------------------------------------------

struct LibraryUnits {
    std::map<std::string, const DesignUnit*> entities;
    std::map<std::string, std::vector<const DesignUnit*>> architectures; ///< by entity key, analysis order
    std::map<std::string, const DesignUnit*> packages;
    std::map<std::string, const DesignUnit*> packageBodies;
    std::map<std::string, const DesignUnit*> configurations;
    std::map<std::string, const DesignUnit*> contexts;
};

/// A file of a VHDL compilation: its library and parsed contents.
struct SourceFileEntry {
    std::string path;     ///< UTF-8
    std::string library;  ///< logical library (lower case)
    std::string text;
    DesignFile ast;
};

// ---------------------------------------------------------------------------
// Process / subprogram execution state
// ---------------------------------------------------------------------------

/// Map that iterates in insertion order (deterministic IR).
template <class K, class V>
class OrderedMap {
public:
    V* find(const K& k) {
        auto it = index_.find(k);
        return it == index_.end() ? nullptr : &items_[it->second].second;
    }
    const V* find(const K& k) const {
        auto it = index_.find(k);
        return it == index_.end() ? nullptr : &items_[it->second].second;
    }
    V& operator[](const K& k) {
        auto [it, inserted] = index_.try_emplace(k, items_.size());
        if (inserted)
            items_.emplace_back(k, V{});
        return items_[it->second].second;
    }
    [[nodiscard]] auto begin() const { return items_.begin(); }
    [[nodiscard]] auto end() const { return items_.end(); }
    [[nodiscard]] bool empty() const { return items_.empty(); }

private:
    std::vector<std::pair<K, V>> items_;
    std::unordered_map<K, std::size_t> index_;
};

using BitMask = std::vector<bool>;

struct ProcState {
    OrderedMap<int, SVal> cur;           ///< variables: current value
    OrderedMap<int, ir::SigSpec> nba;    ///< signals: scheduled next value
    OrderedMap<int, BitMask> any;        ///< bits written on some path
    OrderedMap<int, BitMask> all;        ///< bits written on every path
    ir::SigSpec alive;                   ///< condition under which statements execute (1 bit)
    ir::SigSpec returned;                ///< a return has executed (1 bit)
    struct LoopFrame {
        std::string label;
        ir::SigSpec aliveStart;          ///< alive when the loop was entered
        ir::SigSpec exit;                ///< exit taken (stays set)
        ir::SigSpec skip;                ///< next taken in the current iteration
    };
    std::vector<LoopFrame> loops;
    SVal retValue;                       ///< function result so far
    bool hasWait = false;
};

/// A signal, port or variable of the body being built, or a local variable /
/// parameter of an inlined subprogram (no storage).
struct Object {
    std::string name;
    const Type* type = nullptr;
    bool isSignal = false;
    bool isPort = false;
    Mode mode = Mode::None;
    ir::SigSpec storage;           ///< canonical bits over the object's nets (empty: no hardware)
    SVal initial;                  ///< variables: value at the start of an activation
    bool readInitial = false;      ///< process variable: its stored value was read
    Loc loc;
};

/// Context of one evaluation.
struct Ctx {
    Scope* scope = nullptr;
    ProcState* st = nullptr;       ///< inside a process or inlined subprogram
    bool constOnly = false;        ///< no hardware may be created
    int inlineDepth = 0;
    const Type* returnType = nullptr;
};

/// Assignment destination: bits [offset, offset+width) of an object's
/// canonical value, or a dynamically indexed element.
struct Target {
    int object = -1;
    std::uint32_t offset = 0;
    std::uint32_t width = 0;
    const Type* type = nullptr;    ///< type of the targeted part
    bool dynamic = false;
    ir::SigSpec index;             ///< dynamic: index value
    bool indexSigned = false;
    std::uint32_t elemWidth = 0;
    std::uint32_t regionOffset = 0;
    std::uint32_t regionWidth = 0;
    IntRange range;                ///< dynamic: index range of the region
    bool sub = false;              ///< dynamic: only part of the element ([subOffset, +width))
    std::uint32_t subOffset = 0;
};

// ---------------------------------------------------------------------------
// Elaborator
// ---------------------------------------------------------------------------

/// Possible types of an expression before overload resolution.
struct TypeSet {
    std::vector<const Type*> types;
    bool univInt = false;
    bool univReal = false;
    bool anyString = false;
    bool anyAggregate = false;
    bool anyNull = false;
    [[nodiscard]] bool empty() const noexcept {
        return types.empty() && !univInt && !univReal && !anyString && !anyAggregate && !anyNull;
    }
    void add(const Type* t);
    void merge(const TypeSet& o);
    [[nodiscard]] bool accepts(const Type& t) const;
};

/// Candidate of overload resolution.
struct Candidate {
    Symbol* sym = nullptr;           ///< null for predefined operators
    Op predef = Op::None;            ///< predefined operator
    std::vector<const Type*> params; ///< parameter types
    const Type* result = nullptr;
};

struct ElabOptions {
    std::string top;                 ///< entity name, "lib.entity" or "entity(arch)"
    std::uint32_t maxLoopIterations = 1U << 20;
    std::uint32_t maxInlineDepth = 64;
    /// Generic overrides of the top entity (name, VHDL expression).
    std::vector<std::pair<std::string, std::string>> generics;

    // ---- mixed-language designs ----
    /// Design to add the bodies to (shared with the Verilog frontend); null: a new one.
    std::shared_ptr<ir::Design> design;
    /// Modules of the other language: lower-case name -> name as written.
    std::map<std::string, std::string> foreignModules;
    /// Body of a foreign module for these parameter values; invalid if not
    /// available yet (it is then requested for the next elaboration round).
    std::function<ir::ModuleId(const std::string& module, const std::vector<ir::InstanceParam>& params)>
        resolveForeign;
    /// Entities that must not be inferred as tops (instantiated from Verilog).
    std::set<std::string> excludeTops;
};

/// One unique elaborated body (entity + architecture + generic values).
struct BodyKey {
    std::string library;
    std::string entity;
    std::string architecture;
    std::string generics; ///< canonical generic values
    friend bool operator<(const BodyKey& a, const BodyKey& b) {
        return std::tie(a.library, a.entity, a.architecture, a.generics) <
               std::tie(b.library, b.entity, b.architecture, b.generics);
    }
};

/// Reads and parses VHDL files into their libraries (missing / empty
/// library: "work"). Syntax messages go to `sink`; the value is false if
/// any file has syntax errors.
Result<bool> loadSources(const std::vector<std::filesystem::path>& paths, const std::vector<std::string>& libraries,
                         DiagnosticSink& sink, std::vector<SourceFileEntry>& files);

class Elaborator {
public:
    Elaborator(std::vector<SourceFileEntry>& files, DiagnosticSink& sink, ElabOptions opts);
    ~Elaborator();

    /// Elaborates the top entity and everything below it.
    std::shared_ptr<ir::Design> run(frontend::ElaborationSummary& summary);
    /// Elaborates the selected or inferred top entities into the design
    /// (without adding instance trees); returns their bodies.
    std::vector<ir::ModuleId> elaborateTops();
    [[nodiscard]] std::vector<frontend::DefinitionInfo> definitions() const;

    // ---- mixed-language designs (VhdlMixed.cpp) ----------------------------
    /// A VHDL port as seen from another language.
    struct ForeignPort {
        std::string name;
        ir::PortDir dir = ir::PortDir::In;
        std::uint32_t width = 0;
        std::vector<std::pair<std::size_t, std::uint32_t>> pieces; ///< (child port index, offset)
    };
    /// True if `name` (any case) is an entity of the VHDL libraries.
    [[nodiscard]] bool hasEntity(const std::string& name);
    /// Body of entity `name` (default architecture) for parameter values
    /// written in another language; invalid on errors.
    ir::ModuleId foreignEntity(const std::string& name, const std::vector<ir::InstanceParam>& params,
                               const SourceLocation& where);
    [[nodiscard]] std::vector<ForeignPort> foreignPorts(ir::ModuleId id) const;
    /// Entity keys instantiated anywhere in the VHDL sources.
    [[nodiscard]] std::set<std::string> instantiatedNames() const;

    [[nodiscard]] bool hadErrors() const noexcept { return errors_ > 0; }

    // ---- shared by the implementation files ----------------------------
    void error(const std::string& code, const std::string& msg, Loc l);
    void warn(const std::string& code, const std::string& msg, Loc l, Severity sev = Severity::Warning);
    void unsupported(const std::string& code, const std::string& what, Loc l);
    [[nodiscard]] SourceLocation srcLoc(Loc l) const;

    TypeArena types;
    std::deque<Symbol> symbols;
    std::deque<Scope> scopes;
    Symbol* newSymbol(SymKind k, const std::string& key, const std::string& display, Loc l);
    Scope* newScope(Scope* parent, std::string label = {});

    // predefined types (std.standard, ieee)
    struct Std {
        const Type* boolean = nullptr;
        const Type* bit = nullptr;
        const Type* character = nullptr;
        const Type* severity = nullptr;
        const Type* integer = nullptr;
        const Type* natural = nullptr;
        const Type* positive = nullptr;
        const Type* real = nullptr;
        const Type* time = nullptr;
        const Type* string = nullptr;
        const Type* bitVector = nullptr;
        const Type* booleanVector = nullptr;
        const Type* integerVector = nullptr;
        const Type* univInt = nullptr;
        const Type* univReal = nullptr;
        const Type* stdUlogic = nullptr;
        const Type* stdLogic = nullptr;
        const Type* stdUlogicVector = nullptr;
        const Type* stdLogicVector = nullptr;
        const Type* unsignedT = nullptr;
        const Type* signedT = nullptr;
        const Type* bitUnsigned = nullptr;
        const Type* bitSigned = nullptr;
        const Type* arithUnsigned = nullptr;
        const Type* arithSigned = nullptr;
    } std_;

    // ---- library & packages (VhdlScope.cpp) ------------------------------
    void indexLibraries();
    PackageInfo* findPackage(const std::string& lib, const std::string& name, Loc l);
    void elaboratePackage(PackageInfo& p);
    void elaboratePackageBody(PackageInfo& p);
    void applyContext(Scope& scope, const std::vector<ContextItem>& items, const std::string& workLib);
    void declare(Scope& scope, const Decl& d, Ctx& ctx);
    void declareInterface(Scope& scope, const InterfaceDecl& d, Ctx& ctx);
    const Type* elaborateTypeDef(Scope& scope, const Ident& name, const TypeDef& def, Ctx& ctx);
    const Type* subtype(const SubtypeIndication& s, Ctx& ctx);
    const Type* typeMark(const Expr& mark, Ctx& ctx);
    void resolveParams(Symbol& sub);
    void bindSubprogramBody(Scope& scope, Symbol& def);
    std::vector<Symbol*> lookup(Scope& scope, const std::string& key);
    std::vector<Symbol*> lookupSelected(const Expr& name, Ctx& ctx, bool quiet = false);
    std::vector<Symbol*> resolveName(const Expr& name, Ctx& ctx, bool quiet = false);
    LibraryUnits* library(const std::string& name);
    std::map<std::string, LibraryUnits> libraries;
    std::map<std::pair<std::string, std::string>, std::unique_ptr<PackageInfo>> packages;
    Scope* root = nullptr;           ///< holds library symbols
    Scope* standardScope = nullptr;  ///< std.standard (implicitly used everywhere)

    // ---- builtins (VhdlBuiltins.cpp) --------------------------------------
    void createBuiltins();
    bool isBuiltinPackage(const std::string& lib, const std::string& name) const;
    PackageInfo* builtinPackage(const std::string& lib, const std::string& name);
    SVal callBuiltin(Builtin b, const Symbol& sym, std::vector<SVal>& args, const Type* result, Ctx& ctx, Loc l);

    // ---- expressions (VhdlExpr.cpp) ---------------------------------------
    TypeSet typesOf(const Expr& e, Ctx& ctx);
    SVal eval(const Expr& e, Ctx& ctx, const Type* expected);
    std::optional<Value> constValue(const Expr& e, Ctx& ctx, const Type* expected);
    std::optional<std::int64_t> constInt(const Expr& e, Ctx& ctx, const Type* expected = nullptr);
    std::optional<IntRange> discreteRange(const Expr& e, Ctx& ctx, const Type** indexType = nullptr);
    bool targets(const Expr& e, Ctx& ctx, std::vector<Target>& out, const Type** type = nullptr);
    SVal convert(const SVal& v, const Type* to, Ctx& ctx, Loc l, bool implicit = true);
    ir::SigSpec bitsOf(const SVal& v, Loc l);
    SVal makeConst(const Type* t, Value v);
    SVal makeBits(const Type* t, ir::SigSpec bits);
    ir::SigSpec cell(ir::CellKind k, std::vector<ir::SigSpec> in, std::uint32_t w, Loc l, bool isSigned = false);
    ir::SigSpec toBool(const ir::SigSpec& s, Loc l);
    ir::SigSpec mux(const ir::SigSpec& sel, const ir::SigSpec& whenFalse, const ir::SigSpec& whenTrue, Loc l);
    bool resolveOverload(std::vector<Candidate>& cands, const std::vector<TypeSet>& argSets, const Type* expected,
                         const std::string& what, Loc l, Candidate& chosen, bool quiet = false);
    std::vector<Candidate> predefinedCandidates(Op op, const std::vector<TypeSet>& sets, const Type* expected);
    SVal applyPredefined(Op op, std::vector<SVal>& args, const Type* result, Ctx& ctx, Loc l);
    SVal callSubprogram(Symbol& sym, std::vector<SVal>& args, std::vector<std::vector<Target>>& outTargets,
                        const Type* result, Ctx& ctx, Loc l);
    SVal readObject(int obj, Ctx& ctx, Loc l);
    /// Maps actuals to the formals of `sub` (nullptr: default / open);
    /// false if the association list does not fit.
    bool mapArgs(Symbol& sub, const std::vector<Assoc>& args, std::vector<const Expr*>& perFormal);
    /// Overload resolution for a call by name.
    Symbol* chooseSubprogram(const std::vector<Symbol*>& syms, const std::vector<Assoc>& args, const Type* expected,
                             bool functions, Ctx& ctx, Loc l, bool quiet = false);
    /// Evaluates the actuals of a call to `sub` (defaults, out-parameter targets).
    bool evalArgs(Symbol& sub, const std::vector<Assoc>& args, Ctx& ctx, Loc l, std::vector<SVal>& values,
                  std::vector<std::vector<Target>>& outTargets);
    SVal indexValue(const SVal& arr, const std::vector<SVal>& idx, Loc l);
    SVal sliceValue(const SVal& arr, const IntRange& r, Loc l);
    SVal fieldValue(const SVal& rec, std::size_t field, Loc l);
    SVal evalAggregate(const Expr& e, Ctx& ctx, const Type* expected);
    SVal evalStringLiteral(const std::string& text, const Type* expected, Loc l);
    SVal evalAttribute(const Expr& e, Ctx& ctx, const Type* expected);
    SVal evalCall(const Expr& e, Ctx& ctx, const Type* expected);
    SVal evalOperator(const Expr& e, Ctx& ctx, const Type* expected);
    SVal opaque(const Type* t, std::vector<ir::SigSpec> inputs, const std::string& what, Loc l);

    // ---- statements and processes (VhdlStmt.cpp) --------------------------
    void exec(const Stmt& s, Ctx& ctx);
    void execList(const std::vector<StmtPtr>& list, Ctx& ctx);
    void assign(const std::vector<Target>& targets, const SVal& value, bool signal, Ctx& ctx, Loc l);
    void mergeStates(const ir::SigSpec& cond, ProcState& t, ProcState& f, ProcState& out, Loc l);
    void lowerProcess(const Stmt& p, Scope& scope);
    void lowerConcurrentAssign(const Stmt& s, Scope& scope);
    SVal initialValue(const Object& o, Loc l);
    std::optional<std::pair<ir::SigSpec, ir::Edge>> clockEdge(const Expr& e, Ctx& ctx, const Expr** rest);

    // ---- bodies & hierarchy (VhdlElab.cpp) --------------------------------
    struct BodyState {
        ir::ModuleBody body;
        std::unique_ptr<ir::BodyEditor> ed;
        std::vector<Object> objects;
        std::string prefix;          ///< generate/block name prefix ("g[2].")
        std::string entityKey;
        std::unordered_map<std::uint32_t, std::uint32_t> netOwner; ///< net -> object (for attributes)
        int processCounter = 0;
    };
    BodyState* body = nullptr;       ///< body under construction
    std::vector<Object> frameObjects; ///< subprogram locals (no storage); indices offset by kFrameBase
    static constexpr int kFrameBase = 1 << 24;
    static constexpr std::uint32_t kMaxObjectBits = 1U << 24; ///< guards against absurd generic values
    Object& object(int idx);
    int addObject(Object o);
    int createStorageObject(const std::string& name, const Type* t, bool signal, Mode mode, Loc l,
                            const std::vector<ir::Attribute>& attrs, const SVal* init);
    void elaborateDeclarativePart(Scope& scope, const std::vector<DeclPtr>& decls, Ctx& ctx);
    void elaborateConcurrent(const std::vector<StmtPtr>& stmts, Scope& scope);
    void elaborateInstance(const Stmt& s, Scope& scope);
    /// Port of an entity with its (constrained) type for one instantiation.
    struct PortSpec {
        std::string key;
        std::string display;
        Mode mode = Mode::In;
        const Type* type = nullptr;
        const Expr* defaultExpr = nullptr;
        Loc loc;
    };
    /// Child port net -> part of the formal port object (parallel to ModuleBody::ports).
    struct PortPiece {
        std::size_t port = 0;        ///< index into the entity's PortSpec list
        std::uint32_t offset = 0;
        std::uint32_t width = 0;
    };
    /// Declares the entity's generics in a new scope, bound to the generic
    /// map (evaluated in `parent`) or their defaults. `canon` receives the
    /// canonical generic values; `params` the "#(...)" text. Null if a
    /// generic has no (static) value.
    Scope* bindGenerics(const DesignUnit& entity, const std::string& lib, const std::vector<Assoc>* map,
                        const Decl* component, Ctx* parent, Loc l, std::string& canon, std::string& params);
    std::vector<PortSpec> portSpecs(const DesignUnit& entity, Scope* entityScope);
    ir::ModuleId bodyFor(const DesignUnit& entity, const DesignUnit& arch, const std::string& lib, Scope* entityScope,
                         const std::vector<PortSpec>& ports, const std::string& canon, const std::string& params,
                         Loc instLoc);
    const DesignUnit* findEntity(const std::string& lib, const std::string& name, std::string* foundLib);
    const DesignUnit* defaultArchitecture(const std::string& lib, const DesignUnit& entity);
    void elaborateForeignInstance(const Stmt& s, Scope& scope, const std::string& module);
    SVal fromForeign(const ir::InstanceParam& p, const Type* t, Loc l);
    std::optional<ir::InstanceParam> toForeign(const SVal& v);
    const std::vector<ir::InstanceParam>* foreignParams_ = nullptr;
    std::map<std::size_t, std::vector<PortPiece>> portPieces_; ///< by ModuleId index
    std::map<std::size_t, std::vector<PortSpec>> portSpecs_;   ///< by ModuleId index

    std::map<BodyKey, ir::ModuleId> bodies_;
    std::shared_ptr<ir::Design> design_;
    std::vector<SourceFileEntry>& files_;
    DiagnosticSink& sink_;
    ElabOptions opts_;
    int errors_ = 0;
    int depth_ = 0;
    std::unordered_set<std::string> reported_;
};

} // namespace opencdc::vhdl
