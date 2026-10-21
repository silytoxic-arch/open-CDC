#pragma once

#include "cdc/CdcModel.h"
#include "core/Diagnostic.h"
#include "graph/DesignView.h"

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace opencdc::cdc {

/// One upstream source of a register input bit, aggregated per source node.
struct ConeEntry {
    static constexpr std::uint8_t kLogic = 1;  ///< passes combinational logic (other than inverters)
    static constexpr std::uint8_t kSelect = 2; ///< passes a mux select or an array index

    std::uint32_t node = 0;
    std::uint32_t lo = 0; ///< source bit hull [lo, hi]
    std::uint32_t hi = 0;
    std::uint8_t flags = 0;
};

/// Upstream information of one register input bit.
struct BitInput {
    static constexpr std::uint32_t kNone = 0xFFFFFFFFu;
    /// `cone` value meaning "the single direct source, no interned set" (saves
    /// one interned set per plain-wire register bit).
    static constexpr std::uint32_t kDirectCone = 0xFFFFFFFEu;

    std::uint32_t cone = 0;            ///< interned source set id (0 = empty), or kDirectCone
    std::uint32_t directNode = kNone;  ///< set if the bit is a plain wire (or inverter) from one source bit
    std::uint32_t directBit = 0;
    bool directInverted = false;

    [[nodiscard]] bool isDirect() const noexcept { return directNode != kNone; }
};

/// A register input bit that reads a given source node.
struct Consumer {
    std::size_t reg = 0;      ///< global register index
    std::uint32_t bit = 0;    ///< bit of the pin (Data: offset into Q; other pins: 0, async resets: index)
    /// Last pin bit of this record: consecutive pin bits that read the node
    /// in the same way (same source bits and flags, not direct) are merged,
    /// so a wide full-dependency cell does not create bits x sources records.
    std::uint32_t bitEnd = 0;
    Pin pin = Pin::Data;
    std::uint32_t lo = 0;     ///< source bits read (hull)
    std::uint32_t hi = 0;
    std::uint8_t flags = 0;
    bool direct = false;      ///< plain wire/inverter from exactly one source bit
    std::uint32_t directBit = 0;
};

/// Sequential dependency graph (architecture §3.5): for every register
/// input pin bit, the set of upstream storage elements / inputs reached
/// backwards through combinational logic, aliases and the hierarchy.
///
/// Sources are aggregated per node with a bit hull, which keeps the graph
/// linear in design size; single-bit wire connections are kept exact
/// (`BitInput::direct*`) because synchronizer recognition needs them.
class SeqGraph {
public:
    SeqGraph(const graph::DesignView& view, DiagnosticSink& sink);

    [[nodiscard]] const graph::DesignView& view() const noexcept { return view_; }
    [[nodiscard]] const std::vector<SourceNode>& nodes() const noexcept { return nodes_; }
    [[nodiscard]] std::vector<SourceNode> takeNodes() { return std::move(nodes_); }
    /// Node id of a global register (register nodes come first).
    [[nodiscard]] static std::uint32_t registerNode(std::size_t globalReg) noexcept {
        return static_cast<std::uint32_t>(globalReg);
    }

    /// Input bits of a register pin. AsyncReset: one entry per asynchronous control.
    [[nodiscard]] std::span<const BitInput> pinBits(std::size_t globalReg, Pin pin) const;
    /// Sources of an input bit (resolves kDirectCone into `scratch`).
    [[nodiscard]] std::span<const ConeEntry> entries(const BitInput& bi, ConeEntry& scratch) const {
        if (bi.cone == BitInput::kDirectCone) {
            scratch = {bi.directNode, bi.directBit, bi.directBit, 0};
            return {&scratch, 1};
        }
        return cone(bi.cone);
    }
    [[nodiscard]] std::span<const ConeEntry> cone(std::uint32_t id) const {
        return {pool_.data() + offsets_.at(id), offsets_.at(id + 1) - offsets_.at(id)};
    }

    /// Consumers of the given nodes (every register pin bit that reads them;
    /// runs of identical non-direct pin bits are merged, see Consumer::bitEnd).
    /// One linear scan over the graph per call.
    [[nodiscard]] std::unordered_map<std::uint32_t, std::vector<Consumer>>
    consumersOf(const std::vector<std::uint32_t>& nodes) const;

    /// Source set of an arbitrary bit (used to reconstruct paths).
    [[nodiscard]] BitInput inputOf(const graph::BitRef& b);
    /// Cell input bits that a given output bit depends on, with the flags the
    /// dependency adds (kLogic/kSelect). Used for path reconstruction.
    struct CellInput {
        graph::BitRef bit;
        std::uint8_t flags = 0;
    };
    [[nodiscard]] std::vector<CellInput> cellInputs(ir::InstanceId inst, std::size_t cell, std::uint32_t outBit) const;

    [[nodiscard]] std::size_t combLoops() const noexcept { return combLoops_; }
    [[nodiscard]] std::size_t globalRegisterOf(ir::InstanceId inst, std::size_t reg) const {
        return view_.globalRegister(inst, reg);
    }
    /// (instance, local register index) of a global register.
    [[nodiscard]] std::pair<ir::InstanceId, std::size_t> localRegister(std::size_t globalReg) const {
        return regLocal_.at(globalReg);
    }

    /// Bit `index` of `sig` in `inst` when it is a constant 0/1 through the
    /// hierarchy and simple logic (not, and, or, reductions, logic ops, mux).
    [[nodiscard]] std::optional<ir::Logic> constantBit(ir::InstanceId inst, const ir::SigSpec& sig,
                                                       std::uint32_t index, int depth = 0) const;

    /// The register's enable is constant 0: it keeps its initial value and is
    /// no source of anything (e.g. the write side of a read-only RAM port).
    [[nodiscard]] bool neverLoads(std::size_t globalReg) const {
        return globalReg < neverLoads_.size() && neverLoads_[globalReg];
    }

private:
    struct CellKey {
        std::uint32_t inst;
        std::uint32_t cell;
        std::uint32_t bit; ///< kAll for full-dependency cells
        friend bool operator==(const CellKey&, const CellKey&) = default;
    };
    struct CellKeyHash {
        std::size_t operator()(const CellKey& k) const noexcept {
            return (static_cast<std::size_t>(k.inst) * 0x9E3779B97F4A7C15ULL) ^ (static_cast<std::size_t>(k.cell) << 21) ^
                   k.bit;
        }
    };
    struct Memo {
        std::uint8_t state = 0; ///< 0 new, 1 expanded (on stack), 2 done
        BitInput value;
    };
    struct PinRange {
        std::uint32_t data = 0, dataLen = 0;
        std::uint32_t en = 0, enLen = 0;
        std::uint32_t srst = 0, srstLen = 0;
        std::uint32_t arst = 0, arstLen = 0;
    };
    static constexpr std::uint32_t kAll = 0xFFFFFFFFu;

    std::uint32_t nodeFor(const graph::Driver& d);
    std::uint32_t intern(std::vector<ConeEntry>& entries);
    static void accumulate(std::vector<ConeEntry>& acc, ConeEntry e,
                           std::unordered_map<std::uint32_t, std::size_t>& where);
    /// Inputs of all output bits of a cell (wide cells tracked as one unit).
    [[nodiscard]] std::vector<CellInput> allCellInputs(ir::InstanceId inst, std::size_t cell) const;
    static constexpr std::uint32_t kWideCell = 4096;
    CellKey keyFor(ir::InstanceId inst, std::size_t cell, std::uint32_t bit) const;
    /// Resolves a driver to a BitInput, computing cell cones as needed.
    BitInput resolve(const graph::Driver& d);
    struct ResolvedInput {
        CellInput in;
        graph::Driver driver;
    };
    BitInput computeCell(const CellKey& root);
    BitInput combine(const CellKey& k, const std::vector<ResolvedInput>& inputs);

    const graph::DesignView& view_;
    DiagnosticSink& sink_;
    std::vector<SourceNode> nodes_;
    std::unordered_map<graph::BitRef, std::uint32_t, graph::BitRefHash> extraNodes_; ///< inputs/black boxes, keyed by bit 0
    std::vector<std::uint32_t> latchNode_;  ///< global latch -> node

    std::vector<ConeEntry> pool_;                         ///< interned cones, stored back to back
    std::vector<std::uint32_t> offsets_;                  ///< cone id -> start in pool_ (size = cones + 1)
    std::vector<std::uint32_t> nextInBucket_;             ///< cone id -> next cone with the same hash
    std::unordered_map<std::size_t, std::uint32_t> coneIndex_; ///< hash -> first cone id
    std::unordered_map<CellKey, Memo, CellKeyHash> memo_;
    std::unordered_map<CellKey, std::vector<ResolvedInput>, CellKeyHash> pending_; ///< inputs of cells waiting for dependencies

    std::vector<BitInput> bits_;
    std::vector<PinRange> pins_;  ///< per global register
    std::vector<std::pair<ir::InstanceId, std::size_t>> regLocal_;
    std::vector<bool> neverLoads_;
    std::size_t combLoops_ = 0;
    std::vector<std::string> loopSamples_; ///< a few loop cells for the diagnostic
};

} // namespace opencdc::cdc
