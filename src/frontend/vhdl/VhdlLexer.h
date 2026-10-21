#pragma once
// VHDL lexer: VHDL-93/2008 lexical elements, case-insensitive keywords,
// extended identifiers, based and bit-string literals, the tick/character
// literal ambiguity, "--" and "/* */" comments, and translate_off/on
// regions (simulation-only code that synthesis ignores).

#include "frontend/vhdl/VhdlAst.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace opencdc::vhdl {

enum class Tok : std::uint8_t {
    End,
    Identifier,      ///< basic identifier (text as written)
    ExtIdentifier,   ///< \extended identifier\ (text with backslashes)
    Keyword,
    AbstractLiteral, ///< integer or real, decimal or based
    CharLiteral,     ///< 'x' (text with quotes)
    StringLiteral,   ///< "..." (text = contents)
    BitStringLiteral,///< text = expanded digits, value = original
    // delimiters
    Amp, Tick, LParen, RParen, Star, Plus, Comma, Minus, Dot, Slash, Colon, Semicolon,
    Lt, Eq, Gt, Bar, LBracket, RBracket, Question, At, Backquote,
    Arrow,      ///< =>
    DoubleStar, ///< **
    VarAssign,  ///< :=
    Ne,         ///< /=
    Ge,         ///< >=
    Le,         ///< <= (also signal assignment)
    Box,        ///< <>
    Cond,       ///< ??
    MatchEq, MatchNe, MatchLt, MatchLe, MatchGt, MatchGe,
    DLt,        ///< <<
    DGt,        ///< >>
    Invalid,
};

enum class Kw : std::uint8_t {
    None,
    Abs, Access, After, Alias, All, And, Architecture, Array, Assert, Attribute, Begin, Block, Body, Buffer,
    Bus, Case, Component, Configuration, Constant, Disconnect, Downto, Else, Elsif, End, Entity, Exit, File,
    For, Function, Generate, Generic, Group, Guarded, If, Impure, In, Inertial, Inout, Is, Label, Library,
    Linkage, Literal, Loop, Map, Mod, Nand, New, Next, Nor, Not, Null, Of, On, Open, Or, Others, Out,
    Package, Port, Postponed, Procedure, Process, Protected, Pure, Range, Record, Register, Reject, Rem,
    Report, Return, Rol, Ror, Select, Severity, Signal, Shared, Sla, Sll, Sra, Srl, Subtype, Then, To,
    Transport, Type, Unaffected, Units, Until, Use, Variable, Wait, When, While, With, Xnor, Xor,
};

[[nodiscard]] const char* keywordText(Kw k) noexcept;

struct Token {
    Tok kind = Tok::End;
    Kw kw = Kw::None;
    std::string text; ///< identifier/literal text as written (string literals: contents)
    std::string value;///< bit strings: original literal; identifiers: lookup key
    Loc loc;
};

struct LexMessage {
    std::string message;
    Loc loc;
};

struct LexResult {
    std::vector<Token> tokens; ///< always ends with Tok::End
    std::vector<LexMessage> errors;
    std::uint32_t translateOffRegions = 0;
};

/// Lowercases ASCII letters.
[[nodiscard]] std::string lowerAscii(std::string_view s);

/// Expands a bit-string literal (base specifier + digits, VHDL-2008 sizes
/// and signedness) into one character per bit, MSB first. Characters other
/// than digits (e.g. '-', 'Z') are replicated per bit. Returns false (with
/// `error`) on malformed input.
bool expandBitString(std::string_view literal, std::string& bits, std::string& error);

[[nodiscard]] LexResult lex(std::string_view text, std::uint32_t file);

} // namespace opencdc::vhdl
