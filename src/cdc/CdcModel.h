#pragma once

#include "clock/ClockModel.h"
#include "core/Diagnostic.h"
#include "core/SourceLocation.h"
#include "graph/DesignView.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace opencdc::cdc {

/// Where a crossing starts: a storage element or a design input.
enum class SourceKind : std::uint8_t { Register, Latch, PrimaryInput, BlackBox };

struct SourceNode {
    SourceKind kind = SourceKind::Register;
    std::size_t index = 0;   ///< global register / latch index (Register, Latch)
    graph::BitRef first;     ///< bit 0 of the node's net range (Q or port net)
    std::uint32_t width = 1;
    std::uint32_t elemWidth = 0; ///< memories (unpacked arrays): bits per element; 0 otherwise
    std::string name;        ///< "top.u_tx.data" or "top.u_tx.data[3:0]"
    SourceLocation loc;
};

/// Functional input of the destination register that the crossing enters.
enum class Pin : std::uint8_t { Data, Enable, SyncReset, AsyncReset };

enum class SyncType : std::uint8_t {
    None,          ///< no synchronizer
    SingleFlop,    ///< one destination flop, its output used directly
    TwoFlop,
    ThreeFlop,
    MultiFlop,     ///< four or more flops
    QualifiedData, ///< data captured under an enable/select driven by a synchronized control signal
    // Stage 5: protocol-level synchronizers (the flop chain is still described by syncStages)
    Toggle,        ///< single-bit toggle (pulse) synchronizer: the source toggles, the destination synchronizes the level
    Gray,          ///< multi-bit Gray-coded value synchronized bit by bit
    Handshake,     ///< data qualified by a synchronized request whose acknowledge returns through a synchronizer
    AsyncFifo,     ///< memory read qualified by a Gray pointer, with Gray pointers synchronized in both directions
    ResetSync,     ///< asynchronous reset captured by a reset synchronizer (asynchronous assert, synchronous release)
};

/// Three-level synchronizer verdict (see docs/architecture.md §6).
enum class SyncStatus : std::uint8_t {
    NotSynchronized,
    StructureRecognized,         ///< structure found, but a necessary condition failed
    NecessaryConditionsVerified, ///< every statically checkable condition holds
    ProtocolUnverified,          ///< necessary conditions hold; protocol conditions remain (listed)
};

enum class AnalysisStatus : std::uint8_t { Safe, NeedsReview, PotentialRisk, Violation };

enum class CrossingClass : std::uint8_t { SingleBitControl, SingleBitData, MultiBitControl, MultiBitData };

[[nodiscard]] std::string_view toString(SourceKind v) noexcept;
[[nodiscard]] std::string_view toString(Pin v) noexcept;
[[nodiscard]] std::string_view toString(SyncType v) noexcept;
[[nodiscard]] std::string_view toString(SyncStatus v) noexcept;
[[nodiscard]] std::string_view toString(AnalysisStatus v) noexcept;
[[nodiscard]] std::string_view toString(CrossingClass v) noexcept;

/// Condition text of the first-stage logic check (judged by CDC-004).
inline constexpr std::string_view kConditionNoGlitchLogic =
    "the crossing enters the first stage without glitch-prone combinational logic";

/// A checked synchronizer condition and its outcome.
struct Condition {
    std::string text;
    bool satisfied = true;
};

/// One element of the reconstructed path, from source to destination.
struct PathStep {
    std::string name;  ///< hierarchical name, or cell description
    std::string kind;  ///< "register", "net", "port", "input", "logic:<op>"
    SourceLocation loc;
};

struct RuleHit {
    std::string ruleId;  ///< "CDC-001"
    Severity severity = Severity::Warning;
    AnalysisStatus status = AnalysisStatus::Violation;
    std::string message;
    std::string waiver;  ///< id of the waiver that covers this hit ("" = not waived)
};

/// Synchronized signals of different synchronizers that meet again in the
/// destination domain (CDC-005).
struct Reconvergence {
    std::string at;                 ///< register where the synchronized signals meet
    SourceLocation loc;
    std::uint32_t depth = 0;        ///< register levels after the synchronizer outputs (0 = first reader)
    std::vector<std::size_t> paths; ///< other paths (ids) whose synchronized signals meet there (at most 16 listed)
    std::size_t partners = 0;       ///< number of such paths (may exceed the listed ones)
};

/// One clock domain crossing: a source node feeding (some bits of) one pin
/// of one destination register, between unrelated clocks.
struct CdcPath {
    std::size_t id = 0;
    std::uint32_t srcNode = 0;
    std::size_t dstRegister = 0;   ///< global register index
    Pin pin = Pin::Data;

    clock::ClockEdge srcClock;     ///< the pair that makes this a crossing
    clock::ClockEdge dstClock;
    clock::ClockRelation relation = clock::ClockRelation::Unknown;
    std::string srcClockName;
    std::string dstClockName;

    std::string srcName;
    std::string dstName;
    std::string crossingSignal;    ///< first named signal on the destination side
    SourceLocation srcLoc;
    SourceLocation dstLoc;

    std::uint32_t srcBitLo = 0;    ///< source bits involved (hull)
    std::uint32_t srcBitHi = 0;
    std::vector<std::uint32_t> dstBits; ///< destination register bits (offsets into Q) reached
    bool throughLogic = false;     ///< some bit passes combinational logic (other than inverters)
    bool throughSelect = false;    ///< reaches the destination through a mux/index select
    std::uint32_t width = 1;       ///< max(source bits, destination bits)
    CrossingClass cls = CrossingClass::SingleBitData;

    SyncType syncType = SyncType::None;
    std::uint32_t syncStages = 0;
    SyncStatus syncStatus = SyncStatus::NotSynchronized;
    std::vector<Condition> conditions;
    std::vector<std::string> unverified;   ///< protocol conditions that cannot be proven statically
    std::vector<std::string> notes;
    std::vector<std::string> syncRegisters;///< chain stages / qualifier, by name

    std::vector<PathStep> steps;

    // Stage 5 facts (filled by the advanced analysis, judged by the rules)
    bool srcUnclocked = false;        ///< asynchronous reset from an input without clock domain (srcClock unused)
    std::size_t groupSize = 1;        ///< asynchronous reset paths: destination registers grouped into this path
    std::vector<std::string> groupMembers; ///< the first grouped destination registers (by name)
    bool sourceMayPulse = false;      ///< single-bit source register without hold: it can pulse for one cycle
    bool bitsCombined = false;        ///< bit-wise synchronized bits are read together by one register
    std::vector<std::size_t> splitWith; ///< other paths synchronizing other bits of the same source register (at most 16 listed)
    std::size_t splitBits = 0;        ///< number of source bits of that register synchronized separately
    std::vector<std::string> mixedClocks; ///< other source clocks combined with this crossing in its logic
    std::vector<Reconvergence> reconvergences;
    std::size_t structure = static_cast<std::size_t>(-1); ///< index into CdcResult::structures, if any

    std::vector<RuleHit> hits;
    AnalysisStatus status = AnalysisStatus::Safe; ///< from the hits that are not waived
    bool waived = false;           ///< every hit of this path is waived
    std::string fingerprint;       ///< stable id: hash of source, destination and pin names
};

struct CdcStats {
    std::size_t paths = 0;
    std::size_t synchronized = 0;      ///< synchronizer recognized and necessary conditions hold
    std::size_t unsynchronized = 0;
    std::size_t synchronizers = 0;     ///< distinct recognized flop chains (by first stage register)
    std::size_t unknownRelation = 0;   ///< paths between clocks with unknown relation
    std::size_t excludedNoDomainSources = 0;   ///< (source, destination) pairs skipped: source has no clock domain
    std::size_t excludedUnclockedDestinations = 0;
    std::size_t asyncResetCrossings = 0;       ///< (source, register) connections into asynchronous reset pins that cross
    std::size_t resetSynchronizers = 0;        ///< recognized reset synchronizer chains
    std::size_t reconvergencePoints = 0;       ///< registers where synchronized signals reconverge
    std::size_t waivedHits = 0;                ///< rule hits covered by a waiver
    std::size_t combLoops = 0;
};

/// A recognized synchronizer structure, possibly spanning several paths
/// (a handshake has a request, an acknowledge and data paths).
struct SyncStructure {
    std::size_t id = 0;
    SyncType type = SyncType::None;
    std::string name;                    ///< main register (first stage, data register, ...)
    std::string srcClock;
    std::string dstClock;
    SyncStatus status = SyncStatus::NotSynchronized;
    std::vector<std::size_t> paths;      ///< path ids
    std::vector<std::string> registers;  ///< registers that make up the structure
    std::vector<std::string> notes;
};

/// A waiver as applied to one analysis (stage 7): which hits it covered.
struct WaiverRecord {
    std::string id;
    std::string rule;        ///< rule id or glob ("" = any rule)
    std::string fingerprint; ///< path fingerprint ("" = any)
    std::string source;      ///< glob on the source name ("" = any)
    std::string destination; ///< glob on the destination name ("" = any)
    std::string reason;
    std::string author;
    std::string expires;     ///< YYYY-MM-DD, "" = never
    SourceLocation loc;      ///< where the waiver is defined
    bool expired = false;    ///< not applied: expiry date passed
    std::size_t uses = 0;    ///< rule hits it covers
};

/// Result of CDC analysis; filled by the analyzer, then by the rule engine.
struct CdcResult {
    std::vector<SourceNode> nodes;
    std::vector<CdcPath> paths;
    std::vector<SyncStructure> structures;
    std::vector<WaiverRecord> waivers; ///< waivers of the project, with their use counts
    CdcStats stats;
};

} // namespace opencdc::cdc
