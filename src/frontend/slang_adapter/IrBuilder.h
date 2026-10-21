#pragma once
// Private to the slang adapter: converts slang's elaborated AST into the
// OpenCDC Design IR. Not part of any public interface.

#include "core/Diagnostic.h"
#include "core/SourceLocation.h"
#include "ir/BodyEditor.h"
#include "ir/Design.h"

#include <slang/ast/Compilation.h>
#include <slang/ast/Expression.h>
#include <slang/ast/Statement.h>
#include <slang/ast/symbols/BlockSymbols.h>
#include <slang/ast/symbols/CompilationUnitSymbols.h>
#include <slang/ast/symbols/InstanceSymbols.h>
#include <slang/ast/symbols/MemberSymbols.h>
#include <slang/ast/symbols/ValueSymbol.h>
#include <slang/text/SourceManager.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace opencdc::frontend::detail {

namespace sast = slang::ast;

/// Map from symbols to values that iterates in insertion order (pointer-keyed
/// hash maps would make the IR depend on memory layout).
template <class V>
class OrderedSymMap {
public:
    using Key = const sast::ValueSymbol*;

    V* find(Key k) {
        auto it = index_.find(k);
        return it == index_.end() ? nullptr : &items_[it->second].second;
    }
    const V* find(Key k) const {
        auto it = index_.find(k);
        return it == index_.end() ? nullptr : &items_[it->second].second;
    }
    V& operator[](Key k) {
        auto [it, inserted] = index_.try_emplace(k, items_.size());
        if (inserted)
            items_.emplace_back(k, V{});
        return items_[it->second].second;
    }
    [[nodiscard]] auto begin() const { return items_.begin(); }
    [[nodiscard]] auto end() const { return items_.end(); }
    [[nodiscard]] bool empty() const { return items_.empty(); }

private:
    std::vector<std::pair<Key, V>> items_;
    std::unordered_map<Key, std::size_t> index_;
};

using BitMask = std::vector<bool>;

/// Symbolic state of a procedural block during execution.
struct ProcState {
    OrderedSymMap<ir::SigSpec> cur; ///< values visible to reads (blocking semantics)
    OrderedSymMap<ir::SigSpec> nba; ///< pending non-blocking next values
    OrderedSymMap<BitMask> any;     ///< bits written on at least one path
    OrderedSymMap<BitMask> all;     ///< bits written on every path
    std::unordered_set<const sast::ValueSymbol*> blockingWritten;
    std::unordered_set<const sast::ValueSymbol*> nbaWritten;
    bool returned = false;          ///< function body executed `return`
};

/// slang location -> OpenCDC location (macro expansions map to the point of use).
SourceLocation convertLocation(const slang::SourceManager& sm, slang::SourceLocation loc);

/// One assignment destination: bits [offset, offset+width) of a symbol, or
/// a dynamically indexed element inside region [offset, offset+regionWidth).
struct LTarget {
    const sast::ValueSymbol* sym = nullptr;
    std::uint32_t offset = 0;
    std::uint32_t width = 0;
    bool dynamic = false;
    ir::SigSpec index;              ///< declared index of the least significant written element
    std::uint32_t unitWidth = 0;    ///< width of one indexable element (index granularity)
    bool indexSigned = false;
    std::uint32_t regionWidth = 0;
    std::int32_t left = 0;
    std::int32_t right = 0;
};

inline LTarget wholeTarget(const sast::ValueSymbol* sym, std::uint32_t width) {
    LTarget t;
    t.sym = sym;
    t.width = width;
    return t;
}

class IrBuilder;

/// Builds one ModuleBody from one (canonical) slang instance body.
class BodyBuilder {
public:
    BodyBuilder(IrBuilder& owner, const sast::InstanceBodySymbol& body);
    ir::ModuleBody build();
    [[nodiscard]] std::unordered_map<const sast::Symbol*, ir::NetId> takeNetMap() { return std::move(netOf_); }

    // ---- shared helpers (IrBuilder.cpp) ----
    ir::SigSpec lower(const sast::Expression& e);
    std::optional<std::vector<LTarget>> lvalue(const sast::Expression& e);
    ir::SigSpec cell(ir::CellKind kind, std::vector<ir::SigSpec> inputs, std::uint32_t width,
                     slang::SourceLocation loc, bool isSigned = false);
    ir::SigSpec emit(ir::Cell c);
    ir::SigSpec toBool(const ir::SigSpec& s, slang::SourceLocation loc);
    ir::SigSpec opaque(const sast::Expression& e, const std::string& what);
    void unsupported(const std::string& code, const std::string& what, slang::SourceLocation loc);
    void warn(const std::string& code, Severity sev, const std::string& msg, slang::SourceLocation loc);
    [[nodiscard]] SourceLocation loc(slang::SourceLocation l) const;
    [[nodiscard]] std::uint32_t widthOf(const sast::ValueSymbol& sym) const;
    std::optional<ir::NetId> netFor(const sast::ValueSymbol& sym);

private:
    // declarations and structure (IrBuilder.cpp)
    void declarePorts();
    void declareScope(const sast::Scope& scope);
    void lowerScope(const sast::Scope& scope);
    void lowerMember(const sast::Symbol& member);
    void lowerContinuousAssign(const sast::ContinuousAssignSymbol& ca);
    void lowerNetInitializer(const sast::ValueSymbol& sym);
    void lowerInstance(const sast::InstanceSymbol& inst);
    void lowerPrimitive(const sast::PrimitiveInstanceSymbol& prim);
    void lowerUninstantiated(const sast::UninstantiatedDefSymbol& u);
    std::string relativeName(const sast::Symbol& sym) const;
    bool isModuleLevelStorage(const sast::ValueSymbol& sym) const;

    // SystemVerilog interfaces (IrBuilder.cpp): an interface instance is
    // flattened into the body that declares it ("ia.data"); a module with an
    // interface port gets one port per interface member it uses ("b.data").
    struct IfaceRef {
        const sast::InstanceSymbol* inst = nullptr; ///< the interface instance
        std::string rel;                            ///< member path inside it
    };
    [[nodiscard]] std::optional<IfaceRef> ifaceRef(const sast::Symbol& sym) const;
    /// The variable/net a value reference designates when it is reachable here
    /// (modport ports resolved to the interface member), or nullptr.
    const sast::ValueSymbol* ifaceTarget(const sast::ValueSymbol& sym);
    /// The connection expression of a modport port with an explicit
    /// expression (".g(gray)"), or nullptr.
    [[nodiscard]] const sast::Expression* modportExpression(const sast::ValueSymbol& sym) const;
    void lowerInterfaceInstance(const sast::InstanceSymbol& inst);
    void bindInterfacePort(const sast::PortConnection& pc, ir::ChildInstance& ci, ir::ModuleId cid);
    void finishVirtualPorts();

    // expressions (IrBuilder.cpp)
    ir::SigSpec lowerImpl(const sast::Expression& e);
    ir::SigSpec lowerUnary(const sast::Expression& e);
    ir::SigSpec lowerBinary(const sast::Expression& e);
    ir::SigSpec lowerSelect(const sast::Expression& e);
    ir::SigSpec lowerCall(const sast::Expression& e);
    std::optional<ir::SigSpec> tryConstant(const sast::Expression& e);
    bool lvalueInto(const sast::Expression& e, std::vector<LTarget>& out);

public:
    // procedural lowering (ProcessLowering.cpp)
    ir::SigSpec read(const sast::ValueSymbol& sym);
    void assignTargets(const std::vector<LTarget>& targets, const ir::SigSpec& value, bool nonBlocking,
                       slang::SourceLocation l);
    ir::SigSpec inlineFunction(const sast::Expression& call);

private:
    void lowerProcedure(const sast::ProceduralBlockSymbol& pb);
    void lowerInitial(const sast::Statement& body, slang::SourceLocation l);
    void lowerComb(const sast::Statement& body, bool intendedLatch, slang::SourceLocation l);
    void lowerSequential(const sast::Statement& body,
                         std::vector<std::pair<ir::Edge, const sast::Expression*>> events, slang::SourceLocation l);
    void exec(const sast::Statement& s, ProcState& st);
    void execExpressionStatement(const sast::Expression& e, ProcState& st);
    void execCase(const sast::Statement& s, ProcState& st);
    void execFor(const sast::Statement& s, ProcState& st);
    void merge(const ir::SigSpec& cond, ProcState& thenSt, ProcState& elseSt, ProcState& out,
               slang::SourceLocation l);
    ir::SigSpec defaultValue(const sast::ValueSymbol& sym);
    ir::SigSpec nbaBase(const ProcState& st, const sast::ValueSymbol& sym);
    void extractControls(ir::Register& r, slang::SourceLocation l);
    std::optional<std::pair<ir::SigSpec, bool>> resetCondition(const sast::Expression& cond);

    IrBuilder& owner_;
    const sast::InstanceBodySymbol& sym_;
    ir::ModuleBody body_;
    std::string prefix_; ///< hierarchical path prefix of symbols in this body ("top.u_rx.")
    std::unordered_map<const sast::Symbol*, ir::NetId> netOf_;
    ir::BodyEditor ed_{body_}; ///< cell creation, control extraction, cleanup (shared with the VHDL frontend)
    std::unordered_set<const sast::ValueSymbol*> loopVars_;     ///< variables used as for-loop indices
    ProcState* proc_ = nullptr;                                 ///< non-null inside procedural code
    int inlineDepth_ = 0;
    const sast::ValueSymbol* retVar_ = nullptr;                 ///< return variable of the function being inlined
    std::unordered_map<const sast::Symbol*, std::string> portOfIface_; ///< interface instance -> port name
    struct VirtualPort {
        ir::NetId net;
        std::string name;
    };
    std::vector<VirtualPort> virtualPorts_;
    std::unordered_map<const sast::Symbol*, ir::PortDir> modportDir_; ///< interface member -> modport direction
    std::unordered_set<std::uint32_t> drivenByChild_;                 ///< nets driven through a child's interface port
};

/// Drives the conversion of the whole design.
class IrBuilder {
public:
    IrBuilder(const slang::SourceManager& sm, DiagnosticSink& sink);
    /// Definitions provided by another language (mixed designs): instances of
    /// them are kept as unresolved children with their parameters and ports.
    void setForeignDefinitions(std::set<std::string> names, bool mixed) {
        foreign_ = std::move(names);
        mixed_ = mixed;
    }
    [[nodiscard]] bool mixed() const noexcept { return mixed_; }
    [[nodiscard]] const std::set<std::string>& foreignDefinitions() const noexcept { return foreign_; }

    std::shared_ptr<ir::Design> build(const sast::RootSymbol& root);

    // ---- used by BodyBuilder ----
    ir::ModuleId bodyFor(const sast::InstanceSymbol& inst);
    [[nodiscard]] const std::unordered_map<const sast::Symbol*, ir::NetId>& netMap(ir::ModuleId id) const {
        return netMaps_.at(id.index());
    }
    [[nodiscard]] SourceLocation loc(slang::SourceLocation l) const;
    void report(Diagnostic d) { sink_.report(std::move(d)); }
    DiagnosticSink& sink() { return sink_; }
    ir::Design& design() { return *design_; }

private:
    const slang::SourceManager& sm_;
    DiagnosticSink& sink_;
    std::shared_ptr<ir::Design> design_;
    std::unordered_map<const sast::InstanceBodySymbol*, ir::ModuleId> built_;
    std::vector<std::unordered_map<const sast::Symbol*, ir::NetId>> netMaps_;
    int depth_ = 0;
    std::set<std::string> foreign_;
    bool mixed_ = false;
};

} // namespace opencdc::frontend::detail
