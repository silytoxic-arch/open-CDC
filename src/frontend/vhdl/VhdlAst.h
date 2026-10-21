#pragma once
// VHDL abstract syntax tree (VHDL-93 / VHDL-2008, synthesizable subset plus
// enough of the rest to parse real code). The parser builds it; semantic
// analysis interprets it. Names are kept generic (prefix + suffixes) because
// VHDL cannot tell an indexed name, a slice, a function call and a type
// conversion apart without semantic information.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace opencdc::vhdl {

/// Compact source position: file index into the frontend's file table.
struct Loc {
    std::uint32_t file = 0;
    std::uint32_t line = 0;
    std::uint32_t col = 0;
    [[nodiscard]] bool valid() const noexcept { return line > 0; }
};

/// An identifier as written, plus its lookup key: lower case for basic
/// identifiers, the exact text (with backslashes) for extended identifiers,
/// the quoted text for character literals used as enumeration literals and
/// the quoted operator for operator symbols ("\"and\"").
struct Ident {
    std::string text;
    std::string key;
    Loc loc;
    [[nodiscard]] bool empty() const noexcept { return key.empty(); }
};

struct Expr;
struct Stmt;
struct Decl;
struct SubtypeIndication;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;
using DeclPtr = std::unique_ptr<Decl>;
using SubtypePtr = std::unique_ptr<SubtypeIndication>;

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

enum class ExprKind : std::uint8_t {
    // names
    Simple,      ///< ident
    Selected,    ///< prefix.ident ("all" suffix: ident.key == "all")
    Attribute,   ///< prefix'ident [ (args[0]) ]
    Call,        ///< prefix(args): index, slice, function call or type conversion
    External,    ///< VHDL-2008 << signal .a.b : t >> (not analyzed)
    // literals
    Integer,     ///< text, value
    Real,
    Physical,    ///< lhs = abstract literal (may be null: "ns" alone), ident = unit
    Character,   ///< ident.text = "'a'"
    String,      ///< text = contents (doubled quotes resolved)
    BitString,   ///< text = expanded digits ('0','1', or other characters kept)
    Null,        ///< the literal null
    // composite
    Aggregate,   ///< elements
    Qualified,   ///< prefix'(operand) : prefix = type mark, lhs = operand
    Unary,       ///< op lhs
    Binary,      ///< lhs op rhs
    Range,       ///< lhs to/downto rhs
    Subtype,     ///< a subtype indication in a discrete-range position
    Others,      ///< choice "others"
    Open,        ///< actual "open"
    Box,         ///< "<>" in index subtype definitions / interface defaults
    Inertial,    ///< VHDL-2008 "inertial expr" in port maps: lhs = expr
};

enum class Op : std::uint8_t {
    None,
    // logical
    And, Or, Nand, Nor, Xor, Xnor,
    // relational
    Eq, Ne, Lt, Le, Gt, Ge,
    MatchEq, MatchNe, MatchLt, MatchLe, MatchGt, MatchGe, // ?= ?/= ?< ?<= ?> ?>=
    // shift
    Sll, Srl, Sla, Sra, Rol, Ror,
    // adding
    Add, Sub, Concat,
    // multiplying
    Mul, Div, Mod, Rem,
    // misc
    Pow, Abs, Not,
    // unary
    Plus, Minus,   ///< sign
    Condition,     ///< ?? (VHDL-2008)
    // unary reduction (VHDL-2008): and/or/... used as unary operators
    RedAnd, RedOr, RedNand, RedNor, RedXor, RedXnor,
};

[[nodiscard]] const char* opSymbol(Op op) noexcept;

/// Association element "formal => actual" (formal may be absent).
struct Assoc {
    ExprPtr formal;
    ExprPtr actual;
    Loc loc;
};

/// Aggregate element "choice | choice => value" (choices empty: positional).
struct ElementAssoc {
    std::vector<ExprPtr> choices;
    ExprPtr value;
    Loc loc;
};

enum class RangeDir : std::uint8_t { To, Downto };

struct Expr {
    ExprKind kind = ExprKind::Simple;
    Loc loc;
    Ident ident;                 ///< Simple / Selected suffix / Attribute designator / Physical unit / Character
    ExprPtr prefix;              ///< Selected / Attribute / Call / Qualified
    std::vector<Assoc> args;     ///< Call arguments; Attribute parameter in args[0]
    std::string text;            ///< literal text (see ExprKind)
    std::int64_t ival = 0;       ///< Integer value (also the value of a Physical's literal if integral)
    double rval = 0.0;           ///< Real value
    bool overflow = false;       ///< integer literal does not fit in 64 bits
    std::uint32_t bitWidth = 0;  ///< BitString: VHDL-2008 explicit length (0 = none)
    Op op = Op::None;            ///< Unary / Binary
    RangeDir dir = RangeDir::To; ///< Range
    ExprPtr lhs;                 ///< Unary operand, Binary/Range left, Qualified operand, Physical literal
    ExprPtr rhs;                 ///< Binary/Range right
    std::vector<ElementAssoc> elements; ///< Aggregate
    SubtypePtr subtype;          ///< Subtype / External type
    bool parenthesized = false;  ///< written as ( expr ): blocks positional-aggregate ambiguity checks

    Expr() = default;
    Expr(ExprKind k, Loc l) : kind(k), loc(l) {}
};

/// [resolution] type_mark [constraint]. An index constraint is part of the
/// mark (a Call expression: std_logic_vector(7 downto 0)); a range
/// constraint ("integer range 0 to 7") is kept separately.
struct SubtypeIndication {
    ExprPtr resolution;          ///< resolution function name or VHDL-2008 element resolution
    ExprPtr mark;
    ExprPtr range;               ///< Range expression (or attribute name such as x'range)
    Loc loc;
};

// ---------------------------------------------------------------------------
// Declarations
// ---------------------------------------------------------------------------

enum class Mode : std::uint8_t { None, In, Out, InOut, Buffer, Linkage };

/// Generic, port or subprogram parameter.
struct InterfaceDecl {
    enum class Class : std::uint8_t { Default, Constant, Signal, Variable, File, Type, Function, Procedure, Package };
    Class cls = Class::Default;
    std::vector<Ident> names;
    Mode mode = Mode::None;
    SubtypePtr subtype;
    ExprPtr init;
    bool bus = false;
    Loc loc;
};

struct RecordField {
    std::vector<Ident> names;
    SubtypePtr subtype;
    Loc loc;
};

struct PhysicalUnit {
    Ident name;
    ExprPtr value; ///< null for the primary unit
};

struct TypeDef {
    enum class Kind : std::uint8_t { Enum, Range, Physical, Array, Record, Access, File, Protected, Incomplete };
    Kind kind = Kind::Enum;
    std::vector<Ident> literals;          ///< Enum (character literals: text "'0'")
    ExprPtr range;                        ///< Range / Physical: Range expression
    std::vector<PhysicalUnit> units;      ///< Physical
    std::vector<ExprPtr> indexes;         ///< Array: discrete ranges, or Subtype exprs with range <> (unconstrained)
    bool unconstrained = false;           ///< Array declared with "range <>"
    SubtypePtr element;                   ///< Array element / Access designated / File element
    std::vector<RecordField> fields;      ///< Record
    std::vector<DeclPtr> protectedDecls;  ///< Protected (not analyzed)
    Loc loc;
};

struct Subprogram;

enum class DeclKind : std::uint8_t {
    Signal,
    Variable,
    SharedVariable,
    Constant,
    File,
    Type,
    Subtype,
    Alias,
    Component,
    AttributeDecl,     ///< attribute name : type_mark;
    AttributeSpec,     ///< attribute name of entities : class is value;
    Subprogram,        ///< declaration or body
    Use,
    PackageInst,       ///< VHDL-2008 package instantiation
    Ignored,           ///< group, disconnect, configuration specification, PSL, ...
};

struct Decl {
    DeclKind kind = DeclKind::Signal;
    Loc loc;
    std::vector<Ident> names;              ///< declared identifiers (one for most kinds)
    SubtypePtr subtype;                    ///< object / subtype / alias subtype / attribute type
    ExprPtr init;                          ///< default value / alias target / attribute value
    std::unique_ptr<TypeDef> type;         ///< Type
    std::vector<InterfaceDecl> generics;   ///< Component
    std::vector<InterfaceDecl> ports;      ///< Component
    std::vector<Ident> entityNames;        ///< AttributeSpec targets ("all"/"others" as keys)
    std::string entityClass;               ///< AttributeSpec: "signal", "entity", ...
    std::unique_ptr<Subprogram> sub;       ///< Subprogram
    std::vector<ExprPtr> useNames;         ///< Use: selected names
    ExprPtr unit;                          ///< PackageInst: uninstantiated package name
    std::vector<Assoc> genericMap;         ///< PackageInst
    std::string what;                      ///< Ignored: what it was
};

struct Subprogram {
    bool isFunction = false;
    bool pure = true;
    Ident designator;                    ///< name or operator symbol ("\"+\"" key)
    std::vector<InterfaceDecl> generics; ///< VHDL-2008 subprogram generics (not analyzed)
    std::vector<InterfaceDecl> params;
    ExprPtr returnType;                  ///< type mark (functions)
    bool hasBody = false;
    std::vector<DeclPtr> decls;
    std::vector<StmtPtr> body;
    Loc loc;
};

// ---------------------------------------------------------------------------
// Statements (sequential and concurrent share one node type)
// ---------------------------------------------------------------------------

enum class StmtKind : std::uint8_t {
    // sequential
    Wait,
    Assert,
    Report,
    SignalAssign,     ///< also VHDL-2008 sequential conditional/selected forms
    VariableAssign,
    ProcCall,
    If,
    Case,
    Loop,
    Next,
    Exit,
    Return,
    Null,
    // concurrent
    Process,
    Block,
    ConcAssign,       ///< concurrent signal assignment (simple, conditional, selected)
    ConcProcCall,
    ConcAssert,
    Instance,
    ForGenerate,
    IfGenerate,
    CaseGenerate,
    Ignored,          ///< PSL directives etc.
};

struct WaveElem {
    ExprPtr value;  ///< null for "null" transactions
    ExprPtr after;
};

/// One alternative of an assignment: value (or waveform) and its condition
/// (conditional form) or choices (selected form).
struct AssignAlt {
    std::vector<WaveElem> wave; ///< variable assignments use wave[0].value
    bool unaffected = false;
    ExprPtr cond;               ///< conditional form; null for the final "else" / simple form
    std::vector<ExprPtr> choices; ///< selected form
    Loc loc;
};

struct IfBranch {
    ExprPtr cond;               ///< null: else
    Ident altLabel;             ///< VHDL-2008 generate alternative label
    std::vector<DeclPtr> decls; ///< generate branches only
    std::vector<StmtPtr> body;
    Loc loc;
};

struct CaseAlt {
    std::vector<ExprPtr> choices;
    Ident altLabel;
    std::vector<DeclPtr> decls; ///< case-generate alternatives
    std::vector<StmtPtr> body;
    Loc loc;
};

struct Stmt {
    StmtKind kind = StmtKind::Null;
    Loc loc;
    Ident label;

    // assignments
    enum class Form : std::uint8_t { Simple, Conditional, Selected };
    Form form = Form::Simple;
    ExprPtr target;             ///< name or aggregate
    std::vector<AssignAlt> alts;
    ExprPtr selector;           ///< Selected form / Case / CaseGenerate
    bool matching = false;      ///< select? / case?
    bool guarded = false;
    bool postponed = false;
    std::string delayMechanism; ///< "", "transport", "inertial", "reject"

    // if / case / generate
    std::vector<IfBranch> branches;
    std::vector<CaseAlt> caseAlts;

    // loops / for-generate
    enum class LoopKind : std::uint8_t { Plain, While, For };
    LoopKind loopKind = LoopKind::Plain;
    Ident loopVar;
    ExprPtr range;              ///< For: discrete range (Range, Subtype or attribute name)
    ExprPtr cond;               ///< While / next / exit / wait until / assert / generate condition
    Ident loopLabel;            ///< next / exit target
    ExprPtr value;              ///< return value / report message / wait for time / assert report
    ExprPtr severity;
    std::vector<ExprPtr> sensitivity; ///< process / wait on
    bool sensitivityAll = false;      ///< process (all)

    // calls
    ExprPtr call;               ///< procedure call name

    // process / block / generate body
    std::vector<DeclPtr> decls;
    std::vector<StmtPtr> body;
    std::vector<InterfaceDecl> generics; ///< block header
    std::vector<InterfaceDecl> ports;    ///< block header
    std::vector<Assoc> genericMap;       ///< instance / block header
    std::vector<Assoc> portMap;

    // instances
    enum class UnitKind : std::uint8_t { Component, Entity, Configuration };
    UnitKind unitKind = UnitKind::Component;
    ExprPtr unit;               ///< component / entity / configuration name (selected for "entity lib.e")
    Ident architecture;         ///< entity lib.e(arch)

    std::string what;           ///< Ignored
};

// ---------------------------------------------------------------------------
// Design units
// ---------------------------------------------------------------------------

struct ContextItem {
    enum class Kind : std::uint8_t { Library, Use, Context };
    Kind kind = Kind::Library;
    std::vector<Ident> libraries; ///< Library
    std::vector<ExprPtr> names;   ///< Use / Context references
    Loc loc;
};

enum class UnitKind : std::uint8_t { Entity, Architecture, Package, PackageBody, PackageInstance, Configuration, Context };

[[nodiscard]] const char* toString(UnitKind k) noexcept;

struct DesignUnit {
    UnitKind kind = UnitKind::Entity;
    Ident name;
    Ident entity;                        ///< Architecture / Configuration: the entity it belongs to
    std::vector<ContextItem> context;
    std::vector<InterfaceDecl> generics; ///< Entity / Package (VHDL-2008 generic packages)
    std::vector<InterfaceDecl> ports;    ///< Entity
    std::vector<DeclPtr> decls;
    std::vector<StmtPtr> stmts;          ///< Entity (passive statements) / Architecture
    ExprPtr instantiated;                ///< PackageInstance: uninstantiated package
    std::vector<Assoc> genericMap;       ///< PackageInstance
    std::string library = "work";        ///< library the unit is analyzed into
    Loc loc;
};

struct DesignFile {
    std::uint32_t file = 0;     ///< index in the file table
    std::vector<DesignUnit> units;
    std::uint32_t translateOffRegions = 0; ///< code skipped by "-- synthesis translate_off"
};

} // namespace opencdc::vhdl
