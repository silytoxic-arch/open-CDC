#pragma once

// Internal header of opencdc_cdc (not part of the public interface): the
// state shared by the analysis passes of CdcAnalyzer.cpp (stage 4: crossings,
// flop chains, qualified data) and AdvancedAnalysis.cpp (stage 5: reset
// synchronizers, protocol synchronizers, reconvergence and related facts).

#include "cdc/CdcAnalyzer.h"
#include "cdc/SeqGraph.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace opencdc::cdc::detail {

inline constexpr std::uint32_t kMaxStages = 8;

using StageRef = std::pair<std::size_t, std::uint32_t>; ///< (global register, bit)

/// One synchronizer chain starting at a destination register bit.
struct Chain {
    std::vector<StageRef> stages;
    std::vector<std::uint32_t> fanoutIssueStages; ///< stage indexes (0-based) with extra fanout
    std::size_t extraFanoutFirst = 0;             ///< other readers of the first stage
};

/// Output (stage >= 2) of a recognized flop-chain synchronizer.
struct SyncOut {
    clock::ClockEdge srcClock;
    std::string name;
    std::size_t path = 0; ///< path whose chain produces this output
};

/// A reset synchronizer: flops of one clock whose first stage has a constant
/// data input and that share one asynchronous reset.
struct ResetChain {
    std::vector<StageRef> stages;
    std::vector<Condition> conditions;
    std::size_t protectedRegisters = 0; ///< registers whose asynchronous reset is driven by the last stage
    [[nodiscard]] bool ok() const {
        for (const auto& c : conditions)
            if (!c.satisfied)
                return false;
        return true;
    }
};

struct State {
    State(const graph::DesignView& v, const clock::ClockModel& c, const constraints::ConstraintSet& k,
          const CdcOptions& o, DiagnosticSink& s)
        : view(v), clocks(c), cons(k), opt(o), sink(s), sg(v, s), result(std::make_shared<CdcResult>()) {}

    const graph::DesignView& view;
    const clock::ClockModel& clocks;
    const constraints::ConstraintSet& cons;
    const CdcOptions& opt;
    DiagnosticSink& sink;
    SeqGraph sg;
    std::shared_ptr<CdcResult> result;

    std::vector<std::vector<clock::ClockEdge>> domain; ///< per node
    std::vector<bool> allDirect;                       ///< per path: every bit enters by wire/inverter
    std::vector<std::vector<Chain>> chains;            ///< per path (data pin): one chain per destination bit
    std::unordered_map<std::size_t, SyncOut> syncOutputs; ///< synchronizer stage >= 2 register -> source
    std::unordered_map<std::size_t, std::size_t> qualifierPath; ///< QualifiedData path -> path of its qualifier
    std::unordered_map<std::size_t, std::size_t> ackPath; ///< Handshake/AsyncFifo path -> acknowledge / read pointer path
    std::vector<ResetChain> resetChains;
    std::unordered_map<std::size_t, std::size_t> resetChainOf; ///< stage register -> reset chain

    // --- helpers -------------------------------------------------------

    /// Consumers of a register node (must have been queried).
    [[nodiscard]] const std::vector<Consumer>& consumersOf(std::size_t g) const {
        static const std::vector<Consumer> kEmpty;
        auto it = consumers_.find(SeqGraph::registerNode(g));
        return it == consumers_.end() ? kEmpty : it->second;
    }
    /// Queries the consumers of the nodes not queried yet (one graph scan).
    void queryConsumers(const std::vector<std::uint32_t>& nodes) {
        std::vector<std::uint32_t> todo;
        for (const auto n : nodes)
            if (!consumers_.contains(n))
                todo.push_back(n);
        if (todo.empty())
            return;
        auto found = sg.consumersOf(todo);
        for (auto& [node, list] : found)
            consumers_.emplace(node, std::move(list));
        for (const auto n : todo)
            consumers_.try_emplace(n);
    }

    [[nodiscard]] std::optional<graph::BitRef> pinBit(std::size_t g, Pin pin, std::uint32_t bit) const;
    /// Register name, with the bit index when the register is wider than one bit.
    [[nodiscard]] std::string stageName(std::size_t reg, std::uint32_t bit) const;
    /// Register whose data input is constant (no source at all).
    [[nodiscard]] bool isConstantDataFlop(std::uint32_t node) const;
    /// Same clock or synchronous clocks.
    [[nodiscard]] bool related(const clock::ClockEdge& a, const clock::ClockEdge& b) const {
        const auto r = clocks.relation(a.clock, b.clock);
        return r == clock::ClockRelation::Same || r == clock::ClockRelation::Synchronous;
    }
    [[nodiscard]] std::string edgeName(const clock::ClockEdge& e) const {
        std::string s = clocks.clock(e.clock).name;
        if (e.edge == ir::Edge::Neg)
            s += " (negedge)";
        return s;
    }
    [[nodiscard]] bool isRegister(std::uint32_t node) const { return node < view.registerCount(); }

private:
    std::unordered_map<std::uint32_t, std::vector<Consumer>> consumers_;
};

// Stage-5 passes (AdvancedAnalysis.cpp), in the order they run.

/// Before crossings are collected: recognizes reset synchronizer chains.
void findResetSynchronizers(State& st);
/// After classification: synchronizer verdict of asynchronous reset paths.
void classifyResetPaths(State& st);
/// Toggle, Gray, handshake and asynchronous FIFO recognition; pulse sources.
void recognizeProtocols(State& st);
/// Reconvergence (CDC-005), combined bit-wise synchronized bits (CDC-008)
/// and clocks mixed in a crossing's logic (CDC-009).
void analyzeConvergence(State& st);
/// Groups the synchronized paths into CdcResult::structures.
void buildStructures(State& st);

} // namespace opencdc::cdc::detail
