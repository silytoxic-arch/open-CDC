// Stage 8 (T8.1): VHDL lexer and parser.

#include "frontend/vhdl/VhdlLexer.h"
#include "frontend/vhdl/VhdlParser.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace opencdc::vhdl;

namespace {

ParseResult parseOk(const std::string& text) {
    auto r = parseVhdl(text, 0);
    for (const auto& m : r.messages)
        UNSCOPED_INFO(m.loc.line << ":" << m.loc.col << ": " << m.message);
    REQUIRE_FALSE(r.hasErrors());
    return r;
}

const DesignUnit& unit(const ParseResult& r, UnitKind k, std::size_t nth = 0) {
    // Single exit through REQUIRE: code after a FAIL is unreachable for MSVC (C4702).
    const DesignUnit* found = nullptr;
    for (const auto& u : r.file.units)
        if (!found && u.kind == k && nth-- == 0)
            found = &u;
    REQUIRE(found != nullptr);
    return *found;
}

std::string show(const Expr& e); // compact rendering for checks

std::string showArgs(const std::vector<Assoc>& args) {
    std::string s;
    for (const auto& a : args) {
        if (!s.empty())
            s += ", ";
        if (a.formal)
            s += show(*a.formal) + " => ";
        s += show(*a.actual);
    }
    return s;
}

std::string show(const Expr& e) {
    switch (e.kind) {
        case ExprKind::Simple: return e.ident.key;
        case ExprKind::Selected: return show(*e.prefix) + "." + e.ident.key;
        case ExprKind::Attribute:
            return show(*e.prefix) + "'" + e.ident.key + (e.args.empty() ? "" : "(" + showArgs(e.args) + ")");
        case ExprKind::Call: return show(*e.prefix) + "(" + showArgs(e.args) + ")";
        case ExprKind::Integer: return std::to_string(e.ival);
        case ExprKind::Real: return e.text;
        case ExprKind::Physical: return (e.lhs ? show(*e.lhs) + " " : "") + e.ident.key;
        case ExprKind::Character: return e.ident.key;
        case ExprKind::String: return "\"" + e.text + "\"";
        case ExprKind::BitString: return "b\"" + e.text + "\"";
        case ExprKind::Null: return "null";
        case ExprKind::Aggregate: {
            std::string s = "(";
            for (const auto& el : e.elements) {
                if (s.size() > 1)
                    s += ", ";
                for (std::size_t i = 0; i < el.choices.size(); ++i)
                    s += (i ? " | " : "") + show(*el.choices[i]);
                if (!el.choices.empty())
                    s += " => ";
                s += show(*el.value);
            }
            return s + ")";
        }
        case ExprKind::Qualified: return show(*e.prefix) + "'(" + show(*e.lhs) + ")";
        case ExprKind::Unary: return std::string("(") + opSymbol(e.op) + " " + show(*e.lhs) + ")";
        case ExprKind::Binary:
            return "(" + show(*e.lhs) + " " + opSymbol(e.op) + " " + show(*e.rhs) + ")";
        case ExprKind::Range:
            return show(*e.lhs) + (e.dir == RangeDir::To ? " to " : " downto ") + show(*e.rhs);
        case ExprKind::Subtype:
            return show(*e.subtype->mark) + (e.subtype->range ? " range " + show(*e.subtype->range) : "");
        case ExprKind::Others: return "others";
        case ExprKind::Open: return "open";
        case ExprKind::Box: return "<>";
        case ExprKind::Inertial: return "inertial " + show(*e.lhs);
        case ExprKind::External: return "<<external>>";
    }
    return "?";
}

std::string expr(const std::string& text) {
    std::vector<ParseMessage> msgs;
    auto e = parseVhdlExpression(text, msgs);
    for (const auto& m : msgs)
        UNSCOPED_INFO(m.message);
    REQUIRE(msgs.empty());
    REQUIRE(e);
    return show(*e);
}

} // namespace

TEST_CASE("VHDL lexer: keywords, identifiers, literals, comments", "[vhdl][lexer]") {
    auto r = lex("ENTITY Foo_1 \\Ext Id\\ 16#FF# 2#1010_1010# 1.5e3 1E3 'a' x'length \"a\"\"b\" "
                 "X\"F_0\" b\"1010\" 12UX\"F\" 8SX\"F\" D\"10\" -- comment\n"
                 "/* block\n comment */ <= => := /= ?= ?/= ?? <> ** << >>",
                 3);
    REQUIRE(r.errors.empty());
    auto& t = r.tokens;
    CHECK(t[0].kind == Tok::Keyword);
    CHECK(t[0].kw == Kw::Entity);
    CHECK(t[1].kind == Tok::Identifier);
    CHECK(t[1].value == "foo_1");
    CHECK(t[2].kind == Tok::ExtIdentifier);
    CHECK(t[2].text == "\\Ext Id\\");
    CHECK(t[3].kind == Tok::AbstractLiteral);
    CHECK(t[6].text == "1E3");
    CHECK(t[7].kind == Tok::CharLiteral);
    CHECK(t[7].text == "'a'");
    CHECK(t[8].kind == Tok::Identifier); // x
    CHECK(t[9].kind == Tok::Tick);       // x'length: tick, not a character literal
    CHECK(t[11].kind == Tok::StringLiteral);
    CHECK(t[11].text == "a\"b");
    CHECK(t[12].kind == Tok::BitStringLiteral);
    CHECK(t[12].text == "11110000");
    CHECK(t[13].text == "1010");
    CHECK(t[14].text == "000000001111");  // 12UX"F": zero extended
    CHECK(t[15].text == "11111111");      // 8SX"F": sign extended
    CHECK(t[16].text == "1010");          // D"10"
    CHECK(t[17].kind == Tok::Le);
    CHECK(t[17].loc.line == 3);           // after two newlines (comment + block comment)
    CHECK(t[17].loc.file == 3);
    CHECK(t[18].kind == Tok::Arrow);
    CHECK(t[19].kind == Tok::VarAssign);
    CHECK(t[20].kind == Tok::Ne);
    CHECK(t[21].kind == Tok::MatchEq);
    CHECK(t[22].kind == Tok::MatchNe);
    CHECK(t[23].kind == Tok::Cond);
    CHECK(t[24].kind == Tok::Box);
    CHECK(t[25].kind == Tok::DoubleStar);
    CHECK(t[26].kind == Tok::DLt);
    CHECK(t[27].kind == Tok::DGt);
    CHECK(t.back().kind == Tok::End);
}

TEST_CASE("VHDL lexer: bit strings with metavalues and errors", "[vhdl][lexer]") {
    std::string bits, err;
    CHECK(expandBitString("X\"-Z\"", bits, err));
    CHECK(bits == "----ZZZZ");
    CHECK(expandBitString("O\"7\"", bits, err));
    CHECK(bits == "111");
    CHECK_FALSE(expandBitString("4X\"1F\"", bits, err)); // does not fit
    CHECK_FALSE(expandBitString("B\"102\"", bits, err)); // '2' invalid in binary
    CHECK(expandBitString("6SB\"10\"", bits, err));
    CHECK(bits == "111110");
}

TEST_CASE("VHDL lexer: translate_off regions are skipped", "[vhdl][lexer]") {
    auto r = lex("a\n-- synthesis translate_off\nb c\n-- synthesis translate_on\nd\n"
                 "-- pragma translate_off\ne\n-- pragma translate_on\n"
                 "--synopsys translate_off\nf\n--synopsys translate_on\ng",
                 0);
    REQUIRE(r.errors.empty());
    std::string seen;
    for (const auto& t : r.tokens)
        seen += t.text;
    CHECK(seen == "adg");
    CHECK(r.translateOffRegions == 3);

    auto open = lex("a -- synthesis translate_off\nb", 0);
    REQUIRE(open.errors.size() == 1); // not closed
}

TEST_CASE("VHDL lexer: errors are located", "[vhdl][lexer]") {
    auto r = lex("a\n  \"unterminated\nb $ c", 0);
    REQUIRE(r.errors.size() == 2);
    CHECK(r.errors[0].loc.line == 2);
    CHECK(r.errors[0].loc.col == 3);
    CHECK(r.errors[1].loc.line == 3);
}

TEST_CASE("VHDL expressions: precedence and names", "[vhdl][parser]") {
    CHECK(expr("a + b * c") == "(a + (b * c))");
    CHECK(expr("-a * b") == "(- (a * b))");
    CHECK(expr("not a and b") == "((not a) and b)");
    CHECK(expr("a and b and c") == "((a and b) and c)");
    CHECK(expr("a = b or c /= d") == "((a = b) or (c /= d))");
    CHECK(expr("a & b & \"01\"") == "((a & b) & \"01\")");
    CHECK(expr("2 ** n - 1") == "((2 ** n) - 1)");
    CHECK(expr("abs x mod 4") == "((abs x) mod 4)");
    CHECK(expr("x sll 2") == "(x sll 2)");
    CHECK(expr("a ?= b") == "(a ?= b)");
    CHECK(expr("?? en") == "(?? en)");
    CHECK(expr("and v") == "(and v)"); // VHDL-2008 reduction
    CHECK(expr("ieee.numeric_std.to_unsigned(5, 8)") == "ieee.numeric_std.to_unsigned(5, 8)");
    CHECK(expr("v(7 downto 4)") == "v(7 downto 4)");
    CHECK(expr("r.field(3).sub") == "r.field(3).sub");
    CHECK(expr("clk'event and clk = '1'") == "(clk'event and (clk = '1'))");
    CHECK(expr("v'range") == "v'range");
    CHECK(expr("t'high(1)") == "t'high(1)");
    CHECK(expr("integer'image(x)") == "integer'image(x)");
    CHECK(expr("unsigned'(x\"0F\")") == "unsigned'(b\"00001111\")");
    CHECK(expr("std_logic_vector'(a & b)") == "std_logic_vector'((a & b))");
    CHECK(expr("(others => '0')") == "(others => '0')");
    CHECK(expr("(7 downto 4 => '1', 0 | 2 => s, others => '0')") ==
          "(7 downto 4 => '1', 0 | 2 => s, others => '0')");
    CHECK(expr("(a, b, c)") == "(a, b, c)");
    CHECK(expr("(a)") == "a");
    CHECK(expr("10 ns") == "10 ns");
    CHECK(expr("16#FF#") == "255");
    CHECK(expr("1E3") == "1000");
    CHECK(expr("\"and\"(a, b)") == "\"and\"(a, b)");
    CHECK(expr("f(x => 1, y => open)") == "f(x => 1, y => open)");
}

TEST_CASE("VHDL expressions: syntax errors", "[vhdl][parser]") {
    std::vector<ParseMessage> m;
    CHECK_FALSE(parseVhdlExpression("a and b or c", m)); // mixing requires parentheses
    REQUIRE(m.size() == 1);
    m.clear();
    CHECK_FALSE(parseVhdlExpression("a + ", m));
    m.clear();
    CHECK_FALSE(parseVhdlExpression("(others)", m)); // others needs =>
}

TEST_CASE("VHDL design units: entity with generics and ports, architecture", "[vhdl][parser]") {
    auto r = parseOk(R"(
library ieee;
use ieee.std_logic_1164.all, ieee.numeric_std.all;

entity sync2 is
  generic (
    WIDTH  : positive := 8;
    STAGES : natural range 2 to 4 := 2
  );
  port (
    clk   : in  std_logic;
    rst_n : in  std_logic := '1';
    d     : in  std_logic_vector(WIDTH - 1 downto 0);
    q     : out std_logic_vector(WIDTH - 1 downto 0)
  );
end entity sync2;

architecture rtl of sync2 is
  type stages_t is array (0 to STAGES - 1) of std_logic_vector(WIDTH - 1 downto 0);
  signal s : stages_t := (others => (others => '0'));
  attribute ASYNC_REG : string;
  attribute ASYNC_REG of s : signal is "TRUE";
begin
  p_sync : process (clk, rst_n)
  begin
    if rst_n = '0' then
      s <= (others => (others => '0'));
    elsif rising_edge(clk) then
      s <= s(s'high - 1 downto 0) & d;
    end if;
  end process p_sync;
  q <= s(STAGES - 1);
end architecture rtl;
)");
    REQUIRE(r.file.units.size() == 2);
    const auto& e = unit(r, UnitKind::Entity);
    CHECK(e.name.key == "sync2");
    REQUIRE(e.context.size() == 2);
    CHECK(e.context[1].names.size() == 2);
    REQUIRE(e.generics.size() == 2);
    CHECK(e.generics[1].names[0].key == "stages");
    CHECK(show(*e.generics[1].subtype->range) == "2 to 4");
    REQUIRE(e.ports.size() == 4);
    CHECK(e.ports[0].mode == Mode::In);
    CHECK(e.ports[3].mode == Mode::Out);
    CHECK(show(*e.ports[2].subtype->mark) == "std_logic_vector((width - 1) downto 0)");
    CHECK(show(*e.ports[1].init) == "'1'");

    const auto& a = unit(r, UnitKind::Architecture);
    CHECK(a.entity.key == "sync2");
    REQUIRE(a.decls.size() == 4);
    CHECK(a.decls[0]->kind == DeclKind::Type);
    CHECK(a.decls[0]->type->kind == TypeDef::Kind::Array);
    CHECK(a.decls[2]->kind == DeclKind::AttributeDecl);
    CHECK(a.decls[3]->kind == DeclKind::AttributeSpec);
    CHECK(a.decls[3]->entityClass == "signal");
    REQUIRE(a.stmts.size() == 2);
    const auto& p = *a.stmts[0];
    CHECK(p.kind == StmtKind::Process);
    CHECK(p.label.key == "p_sync");
    CHECK(p.sensitivity.size() == 2);
    REQUIRE(p.body.size() == 1);
    CHECK(p.body[0]->kind == StmtKind::If);
    REQUIRE(p.body[0]->branches.size() == 2);
    CHECK(show(*p.body[0]->branches[1].cond) == "rising_edge(clk)");
    CHECK(a.stmts[1]->kind == StmtKind::ConcAssign);
    CHECK(a.stmts[1]->loc.line == 32);
}

TEST_CASE("VHDL statements: sequential forms", "[vhdl][parser]") {
    auto r = parseOk(R"(
architecture a of e is
begin
  process (all)
    variable v : integer range 0 to 15;
  begin
    v := 0;
    l1 : for i in d'range loop
      next l1 when d(i) = '0';
      v := v + 1;
      exit when v = 8;
    end loop l1;
    while v > 0 loop v := v - 1; end loop;
    case sel is
      when "00" | "01" => y <= a;
      when "10" => y <= b;
      when others => null;
    end case;
    case? sel is when "1-" => z <= '1'; when others => z <= '0'; end case?;
    y2 <= a when sel = "00" else b when sel = "01" else c;
    with sel select y3 <= a when "00", b when others;
    q := '1' when en else '0';
    proc_call(a, b);
    report "msg" severity note;
    assert v < 16 report "overflow" severity error;
    wait until rising_edge(clk);
    wait for 10 ns;
    wait;
    (a, b) <= c;
    return;
  end process;
end;
)");
    const auto& body = unit(r, UnitKind::Architecture).stmts[0]->body;
    REQUIRE(body.size() == 16);
    CHECK(body[0]->kind == StmtKind::VariableAssign);
    CHECK(body[1]->kind == StmtKind::Loop);
    CHECK(body[1]->loopKind == Stmt::LoopKind::For);
    CHECK(body[1]->label.key == "l1");
    CHECK(body[1]->body[0]->kind == StmtKind::Next);
    CHECK(body[1]->body[0]->loopLabel.key == "l1");
    CHECK(body[2]->loopKind == Stmt::LoopKind::While);
    CHECK(body[3]->kind == StmtKind::Case);
    CHECK(body[3]->caseAlts.size() == 3);
    CHECK(body[3]->caseAlts[0].choices.size() == 2);
    CHECK(body[3]->caseAlts[2].choices[0]->kind == ExprKind::Others);
    CHECK(body[4]->matching);
    CHECK(body[5]->form == Stmt::Form::Conditional);
    CHECK(body[5]->alts.size() == 3);
    CHECK(body[6]->form == Stmt::Form::Selected);
    CHECK(body[7]->kind == StmtKind::VariableAssign);
    CHECK(body[7]->form == Stmt::Form::Conditional);
    CHECK(body[8]->kind == StmtKind::ProcCall);
    CHECK(body[9]->kind == StmtKind::Report);
    CHECK(body[10]->kind == StmtKind::Assert);
    CHECK(body[11]->kind == StmtKind::Wait);
    CHECK(body[11]->cond);
    CHECK(body[12]->value);
    CHECK(body[14]->target->kind == ExprKind::Aggregate);
    CHECK(body[15]->kind == StmtKind::Return);
    CHECK(unit(r, UnitKind::Architecture).stmts[0]->sensitivityAll);
}

TEST_CASE("VHDL statements: concurrent forms, generates and instances", "[vhdl][parser]") {
    auto r = parseOk(R"(
architecture a of e is
  component c is
    generic (W : integer := 4);
    port (x : in std_logic; y : out std_logic);
  end component;
begin
  u0 : c generic map (W => 8) port map (x => a, y => open);
  u1 : entity work.sub(rtl) port map (a, b);
  u2 : component c port map (x => a, y => b);
  u3 : configuration work.cfg;
  g_for : for i in 0 to 3 generate
    signal t : std_logic;
  begin
    t <= a(i);
    b(i) <= t;
  end generate;
  g_if : if W > 4 generate
    y <= a;
  elsif W = 4 generate
    y <= b;
  else generate
    y <= c;
  end generate g_if;
  g_case : case MODE generate
    when 0 => y <= a;
    when alt1: 1 | 2 => y <= b;
    when others => y <= c;
  end generate;
  y <= a when en = '1' else 'Z';
  with s select y <= a when '0', b when others;
  blk : block
    signal x : bit;
  begin
    x <= '1';
  end block blk;
  assert W > 0 report "bad" severity failure;
  proc(a);
end architecture;
)");
    const auto& a = unit(r, UnitKind::Architecture);
    REQUIRE(a.decls.size() == 1);
    CHECK(a.decls[0]->kind == DeclKind::Component);
    CHECK(a.decls[0]->generics.size() == 1);
    REQUIRE(a.stmts.size() == 12);
    CHECK(a.stmts[0]->kind == StmtKind::Instance);
    CHECK(a.stmts[0]->unitKind == Stmt::UnitKind::Component);
    CHECK(a.stmts[0]->genericMap.size() == 1);
    CHECK(a.stmts[0]->portMap[1].actual->kind == ExprKind::Open);
    CHECK(a.stmts[1]->unitKind == Stmt::UnitKind::Entity);
    CHECK(show(*a.stmts[1]->unit) == "work.sub");
    CHECK(a.stmts[1]->architecture.key == "rtl");
    CHECK(a.stmts[2]->unitKind == Stmt::UnitKind::Component);
    CHECK(a.stmts[3]->unitKind == Stmt::UnitKind::Configuration);
    CHECK(a.stmts[4]->kind == StmtKind::ForGenerate);
    CHECK(a.stmts[4]->decls.size() == 1);
    CHECK(a.stmts[4]->body.size() == 2);
    CHECK(a.stmts[5]->kind == StmtKind::IfGenerate);
    CHECK(a.stmts[5]->branches.size() == 3);
    CHECK_FALSE(a.stmts[5]->branches[2].cond);
    CHECK(a.stmts[6]->kind == StmtKind::CaseGenerate);
    CHECK(a.stmts[6]->caseAlts.size() == 3);
    CHECK(a.stmts[6]->caseAlts[1].altLabel.key == "alt1");
    CHECK(a.stmts[7]->form == Stmt::Form::Conditional);
    CHECK(a.stmts[8]->form == Stmt::Form::Selected);
    CHECK(a.stmts[9]->kind == StmtKind::Block);
    CHECK(a.stmts[10]->kind == StmtKind::ConcAssert);
    CHECK(a.stmts[11]->kind == StmtKind::ConcProcCall);
}

TEST_CASE("VHDL declarations: packages, types, subprograms", "[vhdl][parser]") {
    auto r = parseOk(R"(
package pkg is
  constant N : integer := 4;
  type state_t is (IDLE, RUN, \Done\, '0');
  type word_t is range 0 to 255;
  type mem_t is array (natural range <>) of std_logic_vector(7 downto 0);
  type rec_t is record
    a, b : std_logic;
    v    : unsigned(N - 1 downto 0);
  end record;
  type time2 is range 0 to 1000 units fs; ps = 1000 fs; end units;
  subtype byte_t is std_logic_vector(7 downto 0);
  subtype small_t is integer range 0 to 7;
  subtype rslv is resolved std_ulogic_vector;
  alias msb : std_logic is sig(7);
  function log2(x : natural) return natural;
  function "+" (a, b : rec_t) return rec_t;
  procedure p(signal s : out std_logic; constant v : in integer := 0);
  shared variable sv : integer;
  file f : text;
end package pkg;

package body pkg is
  function log2(x : natural) return natural is
    variable r : natural := 0;
  begin
    while 2 ** r < x loop r := r + 1; end loop;
    return r;
  end function log2;
  function "+" (a, b : rec_t) return rec_t is
  begin
    return a;
  end function "+";
  procedure p(signal s : out std_logic; constant v : in integer := 0) is
  begin
    s <= '1';
  end procedure;
end package body;
)");
    const auto& p = unit(r, UnitKind::Package);
    REQUIRE(p.decls.size() == 15);
    CHECK(p.decls[1]->type->kind == TypeDef::Kind::Enum);
    REQUIRE(p.decls[1]->type->literals.size() == 4);
    CHECK(p.decls[1]->type->literals[2].key == "\\Done\\");
    CHECK(p.decls[1]->type->literals[3].key == "'0'");
    CHECK(p.decls[2]->type->kind == TypeDef::Kind::Range);
    CHECK(p.decls[3]->type->unconstrained);
    CHECK(p.decls[4]->type->kind == TypeDef::Kind::Record);
    CHECK(p.decls[4]->type->fields.size() == 2);
    CHECK(p.decls[5]->type->kind == TypeDef::Kind::Physical);
    CHECK(p.decls[5]->type->units.size() == 2);
    CHECK(p.decls[7]->subtype->range);
    CHECK(p.decls[8]->subtype->resolution);
    CHECK(p.decls[9]->kind == DeclKind::Alias);
    CHECK(p.decls[11]->sub->designator.key == "\"+\"");
    CHECK(p.decls[12]->sub->params.size() == 2);
    CHECK(p.decls[12]->sub->params[0].cls == InterfaceDecl::Class::Signal);
    CHECK(p.decls[13]->kind == DeclKind::SharedVariable);
    const auto& b = unit(r, UnitKind::PackageBody);
    REQUIRE(b.decls.size() == 3);
    CHECK(b.decls[0]->sub->hasBody);
    CHECK(b.decls[0]->sub->body.size() == 2);
}

TEST_CASE("VHDL: configurations, contexts and VHDL-2008 package generics", "[vhdl][parser]") {
    auto r = parseOk(R"(
context ctx is
  library ieee;
  use ieee.std_logic_1164.all;
end context ctx;

context work.ctx;
package gp is
  generic (W : natural);
  constant C : natural := W;
end package;

package gp8 is new work.gp generic map (W => 8);

configuration cfg of top is
  for rtl
    for u0 : sub use entity work.sub(rtl); end for;
    for all : other use configuration work.oc; end for;
  end for;
end configuration;
)");
    CHECK(unit(r, UnitKind::Context).name.key == "ctx");
    const auto& gp = unit(r, UnitKind::Package);
    CHECK(gp.generics.size() == 1);
    REQUIRE(gp.context.size() == 1);
    CHECK(gp.context[0].kind == ContextItem::Kind::Context);
    CHECK(unit(r, UnitKind::PackageInstance).genericMap.size() == 1);
    CHECK(unit(r, UnitKind::Configuration).entity.key == "top");
}

TEST_CASE("VHDL parser: errors are reported with location and parsing recovers", "[vhdl][parser]") {
    auto r = parseVhdl(R"(entity e is
  port (a : in bit);
end;
architecture a of e is
  signal s : bit;
begin
  s <= a and;
  t <= a;
  u : process begin
    x := ;
    y := 1;
  end process;
end;
entity f is end;
)",
                       0);
    REQUIRE(r.hasErrors());
    REQUIRE(r.messages.size() == 2);
    CHECK(r.messages[0].loc.line == 7);
    CHECK(r.messages[1].loc.line == 10);
    REQUIRE(r.file.units.size() == 3); // the later units are still parsed
    const auto& a = r.file.units[1];
    REQUIRE(a.stmts.size() == 2);   // t <= a; and the process
    CHECK(a.stmts[1]->body.size() == 1); // y := 1; recovered after the bad statement
    CHECK(r.file.units[2].name.key == "f");
}

TEST_CASE("VHDL parser: unit-level recovery after a broken unit", "[vhdl][parser]") {
    auto r = parseVhdl("entity e is port (a : in bit) end;\n"
                       "entity f is end entity;\n",
                       0);
    REQUIRE(r.hasErrors());
    REQUIRE(r.file.units.size() == 1);
    CHECK(r.file.units[0].name.key == "f");
}
