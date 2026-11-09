#pragma once

#include "core/Id.h"
#include "core/SourceLocation.h"
#include "graph/DesignView.h"
#include "ir/Design.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opencdc::clock {

using ClockId = Id<struct ClockTag>;

enum class ClockKind : std::uint8_t {
    Primary,     ///< top-level input port reached by register clock pins (not constrained)
    Constraint,  ///< defined by create_clock (port, pin, net, or virtual)
    Generated,   ///< defined by create_generated_clock
    Divider,     ///< register output used as a clock (derived, unconstrained)
    BlackBox,    ///< output of an unresolved instance used as a clock
};

[[nodiscard]] std::string_view toString(ClockKind k) noexcept;

struct Clock {
    std::string name;
    ClockKind kind = ClockKind::Primary;
    graph::BitRef source;              ///< invalid for virtual clocks
    std::string sourceName;            ///< hierarchical name of the source bit
    ClockId master;                    ///< Generated / Divider: the clock it is derived from
    std::optional<double> periodNs;
    /// How the period was obtained for a derived clock: periodNs = basis period
    /// x periodRatio, basis = the master clock's name or "the configured input period"
    /// (CLKIN1_PERIOD and the like, when the input clock has no period).
    std::optional<double> periodRatio;
    std::string periodBasis;
    std::vector<double> waveform;
    SourceLocation definedAt;          ///< constraint line, or declaration of the source net
    std::size_t posedgeRegisters = 0;  ///< registers clocked on the rising edge of this clock
    std::size_t negedgeRegisters = 0;
    std::size_t gatedRegisters = 0;    ///< of which reached through clock gating
    /// Other names constraints may use: Vivado names an MMCM/PLL output clock
    /// after the net on the output pin ("clk_out1_clk_wiz_0").
    std::vector<std::string> aliases;
    [[nodiscard]] std::size_t registers() const noexcept { return posedgeRegisters + negedgeRegisters; }
};

/// The clock's name or one of its aliases matches the glob pattern.
[[nodiscard]] bool clockMatches(std::string_view pattern, const Clock& c);

enum class ClockRelation : std::uint8_t {
    Same,         ///< the same clock
    Synchronous,  ///< derived from a common source (fixed phase relationship)
    Asynchronous, ///< declared asynchronous (set_clock_groups -asynchronous)
    Exclusive,    ///< declared logically/physically exclusive: never active together
    Unknown,      ///< independent sources and no constraint relating them
};

[[nodiscard]] std::string_view toString(ClockRelation r) noexcept;

struct ClockEdge {
    ClockId clock;
    ir::Edge edge = ir::Edge::Pos;
    friend bool operator==(const ClockEdge&, const ClockEdge&) = default;
};

enum class ClockStatus : std::uint8_t {
    Resolved,   ///< at least one clock found
    Constant,   ///< clock pin tied to a constant: the register never toggles
    Unresolved, ///< could not determine the clock (reason given)
};

/// How one register (all its bits) is clocked.
struct RegisterClocking {
    std::vector<ClockEdge> clocks; ///< usually one; several through clock multiplexers
    ClockStatus status = ClockStatus::Unresolved;
    bool gated = false;            ///< clock passes through gating logic
    bool muxed = false;            ///< clock passes through a clock multiplexer
    std::vector<graph::BitRef> gatingSignals;
    std::string reason;            ///< for Unresolved / Constant, or notes
};

/// Result of clock analysis. Immutable after construction.
class ClockModel {
public:
    std::vector<Clock> clocks;
    std::vector<RegisterClocking> registers; ///< indexed by DesignView::globalRegister()
    std::size_t latchCount = 0;               ///< level-sensitive elements (domain not analyzed)
    /// per set_input_delay (parallel to ConstraintSet::inputDelays): the
    /// clock of "-clock [get_clocks -of_objects ...]", resolved after tracing
    std::vector<std::optional<ClockId>> inputDelayClocks;
    /// per path exception (ConstraintSet::pathExceptions): the clocks named
    /// by get_clocks -of_objects on its -from and -to sides
    std::vector<std::pair<std::vector<ClockId>, std::vector<ClockId>>> pathExceptionClocks;

    void initRelations();
    void setRelation(ClockId a, ClockId b, ClockRelation r, std::string reason);
    [[nodiscard]] ClockRelation relation(ClockId a, ClockId b) const;
    [[nodiscard]] const std::string& reason(ClockId a, ClockId b) const;

    /// The clock named `name` (or with that alias); a pattern with * or ? matches the first
    /// clock whose name or alias matches it.
    [[nodiscard]] std::optional<ClockId> find(std::string_view name) const;
    [[nodiscard]] const Clock& clock(ClockId id) const { return clocks.at(id.index()); }
    /// Root of the master chain (itself for non-derived clocks).
    [[nodiscard]] ClockId root(ClockId id) const;

    [[nodiscard]] std::size_t resolvedRegisters() const;
    [[nodiscard]] std::size_t unresolvedRegisters() const;

private:
    std::vector<ClockRelation> rel_;
    std::vector<std::string> relReason_;
};

} // namespace opencdc::clock
