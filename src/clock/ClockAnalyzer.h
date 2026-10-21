#pragma once

#include "clock/ClockModel.h"
#include "constraints/Sdc.h"
#include "core/Diagnostic.h"
#include "graph/DesignView.h"

#include <memory>

namespace opencdc::clock {

/// Clock analysis (stage 3):
///  1. constraint clocks (create_clock / create_generated_clock) are attached
///     to their source bits;
///  2. every register clock pin is traced backwards through aliases, ports,
///     inverters, clock gating and clock multiplexers to its source(s):
///     a constrained net, a top-level input (primary clock), a register
///     output (divider clock) or an unresolved instance (black-box clock);
///  3. the relation of every clock pair is derived: same, synchronous
///     (common source), asynchronous/exclusive (set_clock_groups) or unknown.
///     Different clock names alone never imply "asynchronous".
///
/// Diagnostics: CLK-UNRESOLVED, CLK-CONSTANT, CLK-GATED, CLK-MUX,
/// CLK-RELATION-UNKNOWN, CLK-UNUSED, CLK-LATCHES, CON-UNMATCHED.
[[nodiscard]] std::shared_ptr<const ClockModel> analyzeClocks(const graph::DesignView& view,
                                                              const constraints::ConstraintSet& constraints,
                                                              DiagnosticSink& sink);

} // namespace opencdc::clock
