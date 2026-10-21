#pragma once

// CDC schematic model (architecture §10, stage 6): a small graph of the
// registers, logic and synchronizer stages around one CDC path, with a
// layered layout. No Qt dependency: the GUI only draws the result.

#include "cdc/CdcModel.h"
#include "core/SourceLocation.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace opencdc::schematic {

enum class NodeKind : std::uint8_t { Register, Input, BlackBox, Latch, Memory, Logic, Dummy };

/// What a node is in the CDC picture (bit set).
enum Role : std::uint32_t {
    kSource = 1u << 0,        ///< source of a crossing
    kDestination = 1u << 1,   ///< destination register of a crossing (first synchronizer stage)
    kSyncStage = 1u << 2,     ///< later synchronizer stage
    kQualifier = 1u << 3,     ///< synchronized signal that qualifies data (enable/select)
    kReconvergence = 1u << 4, ///< register where synchronized signals reconverge (CDC-005)
};

struct Point {
    double x = 0;
    double y = 0;
};

struct Node {
    std::size_t id = 0;
    NodeKind kind = NodeKind::Register;
    std::string name;   ///< full hierarchical name (unique key)
    std::string label;  ///< short text drawn in the box
    std::string clock;  ///< clock domain name ("" for logic)
    std::string detail; ///< tooltip text
    SourceLocation loc;
    std::uint32_t roles = 0;
    cdc::AnalysisStatus status = cdc::AnalysisStatus::Safe; ///< worst status of the paths ending here
    std::vector<std::size_t> paths; ///< CDC paths this node belongs to

    // Layout (filled by layout())
    int layer = 0;
    int order = 0;
    double x = 0; ///< top-left
    double y = 0;
    double w = 0;
    double h = 0;
};

enum class EdgeKind : std::uint8_t {
    Path,          ///< along a CDC path, source to first synchronizer stage
    Chain,         ///< between synchronizer stages
    Qualifier,     ///< synchronized qualifier into the destination's enable/select
    Reconvergence, ///< synchronizer output to a reconvergence point (through registers)
};

struct Edge {
    std::size_t from = 0;
    std::size_t to = 0;
    EdgeKind kind = EdgeKind::Path;
    std::string label;              ///< net or pin name
    bool crossesClocks = false;     ///< the edge goes from one clock domain into another
    std::vector<std::size_t> paths; ///< CDC paths using this edge
    std::vector<Point> points;      ///< route, from `from` to `to` (filled by layout())
};

struct Schematic {
    std::string title;
    std::vector<std::size_t> paths; ///< CDC paths drawn
    std::size_t focusPath = 0;      ///< the path the schematic was built for
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    double width = 0;
    double height = 0;
    bool truncated = false; ///< more related paths exist than were drawn

    [[nodiscard]] const Node* find(std::string_view name) const;
};

struct BuildOptions {
    std::size_t maxPaths = 24; ///< related paths (structure, same destination) drawn at most
};

namespace detail {
struct FactoryIndex;
}

/// Builds schematics of many paths of one result: the name and destination
/// indexes are built once (buildForPath() builds a factory per call).
class SchematicFactory {
public:
    explicit SchematicFactory(const cdc::CdcResult& result);
    ~SchematicFactory();
    SchematicFactory(const SchematicFactory&) = delete;
    SchematicFactory& operator=(const SchematicFactory&) = delete;

    [[nodiscard]] Schematic build(std::size_t pathId, const BuildOptions& options = {}) const;

private:
    const cdc::CdcResult& result_;
    std::unique_ptr<detail::FactoryIndex> index_;
};

/// Builds the schematic of one path: the path itself, the other paths into
/// the same destination register pin (so logic that merges several sources
/// is complete), the members of its synchronizer structure (asynchronous
/// FIFO, handshake), the synchronizer stages, qualifier and reconvergence
/// points. Throws std::out_of_range for an unknown path id.
[[nodiscard]] Schematic buildForPath(const cdc::CdcResult& result, std::size_t pathId, const BuildOptions& options = {});

struct LayoutOptions {
    double charWidth = 7.0;   ///< approximate width of one label character
    double nodeHeight = 38.0; ///< register / logic box height
    double minNodeWidth = 64.0;
    double layerGap = 70.0;   ///< horizontal gap between layers
    double nodeGap = 26.0;    ///< vertical gap between nodes of a layer
    int orderingSweeps = 8;   ///< barycenter sweeps (down + up) for crossing reduction
};

/// Layered (Sugiyama-style) layout, left to right: cycle breaking by DFS,
/// longest-path layering (sources pulled next to their readers), dummy
/// nodes on long edges, barycenter crossing reduction, stacked coordinates.
/// Deterministic. Dummy nodes are kept in `nodes` (kind Dummy) so edges can
/// be routed through them; drawing code skips them.
void layout(Schematic& s, const LayoutOptions& options = {});

/// Standalone SVG drawing of a laid-out schematic (same visual language as
/// the GUI: clock-domain fills, crossing edges in the path-status color,
/// destination border = path status). Used by the CLI (--schematic-dir),
/// the GUI SVG export and later the HTML report.
[[nodiscard]] std::string toSvg(const Schematic& s, const cdc::CdcResult& result);

/// Number of edge crossings between adjacent layers (for tests and tuning).
[[nodiscard]] std::size_t countCrossings(const Schematic& s);

} // namespace opencdc::schematic
