#pragma once

#include "core/Id.h"
#include "core/SourceLocation.h"
#include "ir/Const.h"
#include "ir/SigSpec.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace opencdc::ir {

using ModuleId = Id<struct ModuleTag>;     ///< unique elaborated module body
using InstanceId = Id<struct InstanceTag>; ///< node of the instance tree

enum class PortDir : std::uint8_t { None, In, Out, InOut };
enum class NetKind : std::uint8_t {
    Wire,     ///< net declared with a net type (wire, tri, ...)
    Variable, ///< reg / logic / integer variable
    Temp,     ///< created by the IR builder (cell outputs, port expressions)
};

[[nodiscard]] std::string_view toString(PortDir d) noexcept;
[[nodiscard]] std::string_view toString(NetKind k) noexcept;

struct Attribute {
    std::string name;
    std::string value;
    friend bool operator==(const Attribute&, const Attribute&) = default;
};

/// A named signal inside one module body.
struct Net {
    std::string name;            ///< relative to the body, generate scopes included: "g_bit[2].tmp"
    std::uint32_t width = 1;
    std::int32_t msb = 0;        ///< declared range, used to print bit indices as written
    std::int32_t lsb = 0;
    NetKind kind = NetKind::Wire;
    PortDir dir = PortDir::None;
    bool isSigned = false;
    /// Unpacked arrays (memories) are flattened: width = elemWidth * depth.
    std::uint32_t arrayDepth = 0; ///< 0 = not an array
    std::uint32_t elemWidth = 0;
    std::optional<ConstBits> init; ///< declaration initializer (FPGA power-up value)
    std::vector<Attribute> attrs;
    SourceLocation loc;

    /// Declared index of normalized bit `offset` (e.g. 3 for bit offset 3 of [7:0]).
    [[nodiscard]] std::int32_t indexOf(std::uint32_t offset) const noexcept;
    [[nodiscard]] const Attribute* findAttr(std::string_view name) const noexcept;
};

/// Word-level combinational operators.
enum class CellKind : std::uint8_t {
    // unary
    Not, Neg, ReduceAnd, ReduceOr, ReduceXor, LogicNot,
    // binary
    And, Or, Xor, Xnor, Add, Sub, Mul, Div, Mod, Pow, Shl, Shr, Sshr,
    Eq, Ne, EqWild, Lt, Le, Gt, Ge, LogicAnd, LogicOr,
    // other
    Mux,       ///< Y = S ? B : A                  inputs: A, B, S
    DynSelect, ///< Y = A[index S]                 inputs: A, S
    DynInsert, ///< Y = A with element S := D      inputs: A, S, D
    Opaque,    ///< unsupported construct; Y conservatively depends on all inputs
};

[[nodiscard]] std::string_view toString(CellKind k) noexcept;
[[nodiscard]] std::size_t inputCount(CellKind k) noexcept; ///< 0 = variable (Opaque)

/// How output bits depend on input bits (used by the dependency graph).
enum class BitDep : std::uint8_t {
    Bitwise, ///< Y[i] depends on A[i], B[i] (and all of S for Mux)
    Full,    ///< every output bit depends on every input bit
};
[[nodiscard]] BitDep bitDependency(CellKind k) noexcept;

struct Cell {
    CellKind kind = CellKind::Opaque;
    std::vector<SigSpec> inputs; ///< order defined by kind (A, B, S, D)
    NetId output;                ///< the cell drives the whole Temp net `output`
    std::uint32_t width = 0;     ///< output width
    bool isSigned = false;       ///< signed comparison / shift / division
    /// Dynamic select/insert: element width and the declared index range of A.
    std::uint32_t elemWidth = 0;
    std::int32_t rangeLeft = 0;
    std::int32_t rangeRight = 0;
    std::string label;           ///< Opaque: what was not understood
    SourceLocation origin;
};

enum class Edge : std::uint8_t { Pos, Neg };

struct ClockSpec {
    SigSpec signal; ///< 1 bit
    Edge edge = Edge::Pos;
};

/// Asynchronous or synchronous reset/set/load control of a register.
struct ResetSpec {
    SigSpec signal;          ///< 1 bit
    bool activeHigh = true;
    SigSpec value;           ///< value loaded into Q (same width as Q); may contain Q bits = "unaffected"
};

/// An edge-triggered storage element covering contiguous bits of one variable.
struct Register {
    SigSpec q;                          ///< the variable bits held by this register
    SigSpec d;                          ///< next-state value (enable / sync reset already stripped)
    ClockSpec clock;
    std::vector<ResetSpec> asyncResets; ///< in priority order
    std::optional<ResetSpec> syncReset;
    std::optional<SigSpec> enable;      ///< active-high clock enable
    SourceLocation origin;              ///< the always block
};

/// A level-sensitive storage element inferred from an incompletely assigned
/// combinational process (or always_latch).
struct Latch {
    SigSpec q;
    SigSpec d; ///< value function, contains Q itself where the latch holds
    SourceLocation origin;
};

enum class ConnKind : std::uint8_t {
    Assign,       ///< continuous assignment / net declaration assignment
    Process,      ///< combinational always block result
    PortExpr,     ///< port connection expression materialized in the parent
};

/// lhs is driven by rhs (lhs only references nets).
struct Connection {
    SigSpec lhs;
    SigSpec rhs;
    ConnKind kind = ConnKind::Assign;
    SourceLocation origin;
};

struct Port {
    std::string name;
    PortDir dir = PortDir::None;
    NetId net;
};

struct PortBinding {
    std::string port;  ///< port name in the child
    PortDir dir = PortDir::None;
    NetId childNet;    ///< port net inside the child body
    SigSpec actual;    ///< parent-side signal (empty = unconnected)
};

/// Parameter value written at an instance of a module/entity that its own
/// frontend could not resolve (bound later to a body of another language).
struct InstanceParam {
    std::string name;                ///< empty: positional
    std::optional<ConstBits> bits;   ///< integral / vector value
    std::optional<std::string> str;  ///< string value
    std::optional<double> real;      ///< real value
    bool isSigned = false;
};

struct ChildInstance {
    std::string name;  ///< relative to the body: "u_sync" or "g_bit[2].u_sync"
    ModuleId body;     ///< invalid for unknown / unsupported modules
    std::string definition;
    /// Unresolved instance (body invalid): ports are bound by name (empty
    /// name: by position) with direction None and the actual as written.
    std::vector<PortBinding> ports;
    SourceLocation loc;
    std::vector<InstanceParam> unresolvedParams;
};

/// One unique elaborated module body (a module with a specific parameter set).
/// All Ids inside are local to the body.
struct ModuleBody {
    std::string name;        ///< definition name
    std::string params;      ///< "#(W=8, DEPTH=16)" or empty
    SourceLocation loc;
    std::vector<Net> nets;
    std::vector<Cell> cells;
    std::vector<Register> registers;
    std::vector<Latch> latches;
    std::vector<Connection> connections;
    std::vector<Port> ports;
    std::vector<ChildInstance> children;

    NetId addNet(Net net);
    NetId addTemp(std::string name, std::uint32_t width);
    /// Adds a cell driving a fresh Temp net and returns that net as a SigSpec.
    SigSpec addCell(Cell cell);

    [[nodiscard]] const Net& net(NetId id) const { return nets.at(id.index()); }
    [[nodiscard]] Net& net(NetId id) { return nets.at(id.index()); }
    [[nodiscard]] SigSpec netSig(NetId id) const { return SigSpec::net(id, net(id).width); }
    [[nodiscard]] std::optional<NetId> findNet(std::string_view name) const;

private:
    std::uint32_t tempCounter_ = 0;
};

/// Node of the elaborated instance tree. Global (flattened) Ids of objects
/// inside the instance are base + local Id.
struct InstanceNode {
    std::string name;      ///< relative to the parent ("u_rx", "g[1].u_s"); top: definition name
    std::string hierPath;  ///< "top.u_rx.g[1].u_s"
    ModuleId body;
    InstanceId parent;
    std::vector<InstanceId> children;
    std::size_t childIndex = 0; ///< index into the parent's ModuleBody::children
    std::uint64_t netBase = 0;
    std::uint64_t cellBase = 0;
    std::uint64_t registerBase = 0;
};

/// Something the IR builder could not (fully) model. Always reported; listed
/// in reports as "not analyzed".
struct UnsupportedItem {
    std::string code;     ///< IR-UNSUPPORTED-*
    std::string module;   ///< body name
    std::string what;
    SourceLocation loc;
};

struct DesignStats {
    std::size_t uniqueBodies = 0;
    std::size_t instances = 0;
    std::uint64_t nets = 0;          ///< flattened
    std::uint64_t cells = 0;         ///< flattened
    std::uint64_t registers = 0;     ///< flattened register objects
    std::uint64_t registerBits = 0;  ///< flattened
    std::uint64_t latches = 0;       ///< flattened
    std::size_t unsupported = 0;
};

/// The complete elaborated design. Immutable once finalize() has been called.
class Design {
public:
    std::vector<ModuleBody> bodies;
    std::vector<InstanceNode> instances;
    std::vector<InstanceId> tops;
    std::vector<UnsupportedItem> unsupported;

    ModuleId addBody(ModuleBody body);
    [[nodiscard]] const ModuleBody& body(ModuleId id) const { return bodies.at(id.index()); }
    [[nodiscard]] ModuleBody& body(ModuleId id) { return bodies.at(id.index()); }
    [[nodiscard]] const InstanceNode& instance(InstanceId id) const { return instances.at(id.index()); }

    /// Builds the instance tree below `topBody` (recursively via ChildInstance
    /// records) and assigns flattening offsets.
    InstanceId addTop(ModuleId topBody);

    [[nodiscard]] DesignStats stats() const;
    /// Instance with this hierarchical path, if any.
    [[nodiscard]] std::optional<InstanceId> findInstance(std::string_view hierPath) const;

private:
    InstanceId addInstance(std::string name, std::string hierPath, ModuleId body, InstanceId parent,
                           std::size_t childIndex);
    std::uint64_t nextNet_ = 0;
    std::uint64_t nextCell_ = 0;
    std::uint64_t nextReg_ = 0;
};

} // namespace opencdc::ir
