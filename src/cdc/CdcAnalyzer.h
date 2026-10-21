#pragma once

#include "cdc/CdcModel.h"
#include "clock/ClockModel.h"
#include "constraints/Sdc.h"
#include "core/Diagnostic.h"
#include "graph/DesignView.h"

#include <memory>

namespace opencdc::cdc {

/// How crossings between clocks with an unknown relation are treated
/// (mirrors engine::UnknownClockPolicy; the cdc library does not depend on engine).
enum class UnknownRelationPolicy : std::uint8_t { Report, AssumeAsync, AssumeSync };

struct CdcOptions {
    UnknownRelationPolicy unknownRelation = UnknownRelationPolicy::Report;
    std::uint32_t minSyncStages = 2;  ///< flop chains shorter than this are reported by CDC-002
    std::uint32_t maxQualifierDepth = 2; ///< register levels searched between a synchronizer and a data enable
    std::uint32_t reconvergenceDepth = 2; ///< register levels searched after synchronizer outputs (CDC-005)
};

/// CDC analysis: builds the sequential dependency graph, finds every
/// crossing between unrelated clocks (including asynchronous reset pins),
/// groups crossings into paths and recognizes synchronizers: flop chains,
/// synchronizer-qualified data, toggle, Gray, handshake, asynchronous FIFO
/// and reset synchronizers. It also computes the facts the stage-5 rules
/// judge (reconvergence, combined bits, mixed clocks, pulse sources).
/// Rule hits are added afterwards by the rule engine (rules/).
///
/// Diagnostics: GR-COMB-LOOP, CDC-NO-DOMAIN, CDC-INPUT-CLOCK.
[[nodiscard]] std::shared_ptr<CdcResult> analyzeCdc(const graph::DesignView& view, const clock::ClockModel& clocks,
                                                    const constraints::ConstraintSet& constraints,
                                                    const CdcOptions& options, DiagnosticSink& sink);

} // namespace opencdc::cdc
