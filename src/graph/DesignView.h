#pragma once

#include "ir/Design.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace opencdc::graph {

/// One bit of a net inside one instance (flattened addressing).
struct BitRef {
    ir::InstanceId inst;
    ir::NetId net;
    std::uint32_t bit = 0;

    [[nodiscard]] bool valid() const noexcept { return inst.valid() && net.valid(); }
    friend bool operator==(const BitRef&, const BitRef&) = default;
};

struct BitRefHash {
    std::size_t operator()(const BitRef& b) const noexcept {
        return (static_cast<std::size_t>(b.inst.value) * 0x9E3779B97F4A7C15ULL) ^
               (static_cast<std::size_t>(b.net.value) << 20) ^ b.bit;
    }
};

/// What ultimately drives a bit, after following aliases (plain net-to-net
/// assignments) and port connections across the hierarchy.
enum class DriverKind : std::uint8_t {
    Undriven,           ///< nothing drives the bit (or an unconnected input port)
    Constant,           ///< tied to a constant
    PrimaryInput,       ///< input port of a top-level module
    Register,           ///< Q of a register
    Latch,              ///< Q of a latch
    Cell,               ///< output of a combinational cell
    UnresolvedInstance, ///< output of an instance whose module is unknown (black box)
    Loop,               ///< alias loop (a = b; b = a)
};

[[nodiscard]] const char* toString(DriverKind k) noexcept;

struct Driver {
    DriverKind kind = DriverKind::Undriven;
    BitRef bit;              ///< the driven bit at the source (register Q bit, cell output bit, port bit)
    std::uint32_t index = 0; ///< register / latch / cell index in the body of bit.inst
    ir::Logic constant = ir::Logic::X;
};

/// Read-only, flattened view of a Design: driver lookup across hierarchy
/// and hierarchical naming. Construction is linear in the IR size; lookups
/// walk the alias chain (typically a handful of steps).
class DesignView {
public:
    explicit DesignView(const ir::Design& design);

    [[nodiscard]] const ir::Design& design() const noexcept { return design_; }

    /// Follows aliases and port bindings from `b` to the real driver.
    /// `stop` (optional) is called for every bit visited, including `b`;
    /// returning true ends the walk at that bit with kind Undriven and
    /// `bit` = that bit (used to stop at constrained clock nets).
    /// `hops` (optional) receives every bit visited on the way, starting with `b`
    /// (used to show the hierarchical path of a crossing).
    [[nodiscard]] Driver driver(const BitRef& b, const std::function<bool(const BitRef&)>& stop = {},
                                std::vector<BitRef>* hops = nullptr) const;

    /// BitRef of bit `index` of `sig` inside `inst`; nullopt for constant bits.
    [[nodiscard]] std::optional<BitRef> bitOf(ir::InstanceId inst, const ir::SigSpec& sig, std::uint32_t index) const;
    /// Constant value of bit `index` of `sig` (X if not constant).
    [[nodiscard]] static ir::Logic constBit(const ir::SigSpec& sig, std::uint32_t index);

    /// "top.u_rx.data[3]" (bit index omitted for 1-bit nets).
    [[nodiscard]] std::string name(const BitRef& b) const;
    /// Instance node of the given child (by index into the body's children), invalid if unresolved.
    [[nodiscard]] ir::InstanceId child(ir::InstanceId parent, std::size_t childIndex) const;
    [[nodiscard]] const ir::ModuleBody& bodyOf(ir::InstanceId inst) const {
        return design_.body(design_.instance(inst).body);
    }
    /// Global (flattened) register number, for per-register tables.
    [[nodiscard]] std::size_t globalRegister(ir::InstanceId inst, std::size_t regIndex) const {
        return static_cast<std::size_t>(design_.instance(inst).registerBase) + regIndex;
    }
    [[nodiscard]] std::size_t registerCount() const noexcept { return totalRegisters_; }
    /// Global (flattened) latch number.
    [[nodiscard]] std::size_t globalLatch(ir::InstanceId inst, std::size_t latchIndex) const {
        return latchBase_.at(inst.index()) + latchIndex;
    }
    [[nodiscard]] std::size_t latchCount() const noexcept { return totalLatches_; }

private:
    enum class EntryKind : std::uint8_t { Conn, Reg, Latch, Cell, InPort, ChildOut };
    struct Entry {
        std::uint32_t lo;  ///< bit range [lo, hi) of the net
        std::uint32_t hi;
        EntryKind kind;
        std::uint32_t index;  ///< connection / register / latch / cell / child index
        std::uint32_t sub;    ///< ChildOut: port index
        std::uint32_t pos;    ///< position of bit `lo` inside the driving SigSpec (lhs / q / actual)
        std::uint32_t maxHi;  ///< max(hi) over this and all earlier entries of the net (for binary search)
    };
    struct BodyIndex {
        std::vector<std::vector<Entry>> drivers; ///< per net, sorted by lo
    };

    [[nodiscard]] const Entry* find(const BodyIndex& idx, ir::NetId net, std::uint32_t bit) const;

    const ir::Design& design_;
    std::vector<BodyIndex> bodies_;                      ///< per ModuleId
    std::vector<std::vector<ir::InstanceId>> children_;  ///< per InstanceId: child index -> instance
    std::size_t totalRegisters_ = 0;
    std::vector<std::size_t> latchBase_;                 ///< per InstanceId
    std::size_t totalLatches_ = 0;
};

} // namespace opencdc::graph
