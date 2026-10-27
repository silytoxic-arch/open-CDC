#pragma once

#include "core/Diagnostic.h"
#include "core/SourceLocation.h"
#include "graph/DesignView.h"

#include <filesystem>
#include <map>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace opencdc::constraints {

enum class ObjectKind : std::uint8_t { Port, Pin, Net, Clock, Cell };

/// A design object returned by get_ports / get_pins / get_nets / get_clocks.
struct Object {
    ObjectKind kind = ObjectKind::Net;
    std::string name;                ///< matched name (for clocks: the name pattern)
    std::vector<graph::BitRef> bits; ///< resolved bits (empty for clocks and hierarchical cells)
    std::int32_t item = -1;          ///< cells / pins: resolver table index (register, instance or port)
    bool instance = false;           ///< cells: a hierarchical instance (no bits)
    std::string pin{};               ///< flip-flop pins: C, D, Q, CE, CLR, PRE, R, S (bits: the signal on the pin)
};

/// Quartus collections (names with '|' as hierarchy separator, registers
/// named after their HDL signal).
enum class QuartusKind : std::uint8_t {
    Registers, ///< get_registers
    Keepers,   ///< get_keepers: registers and ports
    Nodes,     ///< get_nodes: registers and ports
    Cells,     ///< get_cells: registers and instances
    Pins,      ///< get_pins: instance ports, register pins (clk d q ena clrn prn sclr)
};

/// Resolves object patterns against a design. Patterns use Tcl glob syntax
/// (* and ?), '/' (or '.') as hierarchy separator, and an optional bit
/// select suffix: data[3], data[*].
class IObjectResolver {
public:
    IObjectResolver() = default;
    IObjectResolver(const IObjectResolver&) = delete;
    IObjectResolver& operator=(const IObjectResolver&) = delete;
    virtual ~IObjectResolver() = default;

    virtual std::vector<Object> ports(std::string_view pattern, bool nocase) = 0;
    virtual std::vector<Object> pins(std::string_view pattern, bool hierarchical, bool nocase) = 0;
    virtual std::vector<Object> nets(std::string_view pattern, bool hierarchical, bool nocase) = 0;
    /// Flip-flop cells "<reg>_reg" (bits: Q) and hierarchical instances (no
    /// bits); none by default.
    virtual std::vector<Object> cells(std::string_view /*pattern*/, bool /*hierarchical*/, bool /*nocase*/) {
        return {};
    }
    /// Cells whose full name (or, with `hierarchical`, a trailing part of it)
    /// matches the ECMAScript regular expression; vector registers also match
    /// per bit ("x_reg[3]"). Throws std::regex_error on a bad expression.
    virtual std::vector<Object> cellsRegexp(std::string_view /*regex*/, bool /*hierarchical*/, bool /*nocase*/) {
        return {};
    }
    /// Whether cells can have this property (NAME, PARENT, REF_NAME,
    /// ORIG_REF_NAME, IS_SEQUENTIAL, or an HDL attribute present in the design).
    virtual bool cellPropertyKnown(std::string_view /*prop*/) { return false; }
    /// Value of a property of a cell returned by cells(); nullopt if the cell
    /// does not have it (e.g. REF_NAME of a flip-flop).
    virtual std::optional<std::string> cellProperty(const Object& /*cell*/, std::string_view /*prop*/) {
        return std::nullopt;
    }
    /// Clock bits of the flip-flops of a cell (empty for instances).
    virtual std::vector<graph::BitRef> cellClocks(const Object& /*cell*/) { return {}; }
    /// "instance (definition)" of a black-box instance (a module or IP the
    /// design does not define) whose path the pattern starts with.
    virtual std::optional<std::string> blackBoxContaining(std::string_view /*pattern*/) { return std::nullopt; }

    // Netlist traversal (get_pins/get_nets/get_cells -of_objects)
    /// Pins of a cell: flip-flop pins, or the ports of an instance.
    virtual std::vector<Object> cellPins(const Object& /*cell*/) { return {}; }
    /// Nets on pins (all segments).
    virtual std::vector<Object> pinNets(const Object& /*pin*/) { return {}; }
    /// Flip-flop pins on a net: drivers (Q) and loads (D, C, CE, resets).
    virtual std::vector<Object> netPins(const Object& /*net*/) { return {}; }
    /// The cell of a pin (flip-flop or instance).
    virtual std::vector<Object> pinCells(const Object& /*pin*/) { return {}; }
    /// Quartus-style query: '*' also matches across '|'. A pin path that
    /// continues inside an instance whose internals are not modelled (an IP's
    /// "pll|auto_generated|pll1|clk[0]") resolves to that instance's port.
    virtual std::vector<Object> quartus(std::string_view /*pattern*/, QuartusKind /*kind*/, bool /*nocase*/) {
        return {};
    }
    /// Pin properties: NAME, REF_PIN_NAME, DIRECTION, IS_LEAF, IS_CLOCK,
    /// IS_ENABLE, IS_PRESET, IS_CLEAR, IS_SET, IS_RESET.
    virtual bool pinPropertyKnown(std::string_view /*prop*/) { return false; }
    virtual std::optional<std::string> pinProperty(const Object& /*pin*/, std::string_view /*prop*/) {
        return std::nullopt;
    }
};

/// Resolver over a flattened design (Vivado-style names: instance levels
/// joined with '/', top module name omitted; register pins "<reg>_reg/Q" and
/// "<reg>_reg/C" are mapped to the register output and clock).
class DesignObjectResolver final : public IObjectResolver {
public:
    explicit DesignObjectResolver(const graph::DesignView& view);
    std::optional<std::string> blackBoxContaining(std::string_view pattern) override;

    std::vector<Object> ports(std::string_view pattern, bool nocase) override;
    std::vector<Object> pins(std::string_view pattern, bool hierarchical, bool nocase) override;
    std::vector<Object> cells(std::string_view pattern, bool hierarchical, bool nocase) override;
    std::vector<Object> cellsRegexp(std::string_view regex, bool hierarchical, bool nocase) override;
    bool cellPropertyKnown(std::string_view prop) override;
    std::optional<std::string> cellProperty(const Object& cell, std::string_view prop) override;
    std::vector<graph::BitRef> cellClocks(const Object& cell) override;
    std::vector<Object> cellPins(const Object& cell) override;
    std::vector<Object> pinNets(const Object& pin) override;
    std::vector<Object> netPins(const Object& net) override;
    std::vector<Object> pinCells(const Object& pin) override;
    std::vector<Object> quartus(std::string_view pattern, QuartusKind kind, bool nocase) override;
    bool pinPropertyKnown(std::string_view prop) override;
    std::optional<std::string> pinProperty(const Object& pin, std::string_view prop) override;
    std::vector<Object> nets(std::string_view pattern, bool hierarchical, bool nocase) override;

private:
    struct CellInfo {
        std::string parent;                            ///< instance path, '/'-separated ("" at the top)
        std::string ref;                               ///< instances: definition name
        const std::vector<ir::Attribute>* attrs = nullptr;
        const ir::Register* reg = nullptr;             ///< registers
        ir::InstanceId inst;                           ///< registers
        std::uint32_t qOffset = 0;                     ///< registers: first Q bit of the register (bit of its net)
    };
    /// Pins of register `r` restricted to the Q bits `qbits` (named after `cellName`).
    std::vector<Object> registerPins(std::size_t r, const std::string& cellName, const std::vector<graph::BitRef>& qbits);
    std::string bitCellName(std::size_t r, const graph::BitRef& q) const; ///< "x_reg[3]" or "x_reg"
    void buildLoads();
    void buildInstances();
    struct Named {
        std::string name; ///< full relative name, '/'-separated hierarchy
        graph::BitRef first;
        std::uint32_t width = 1;
        std::int32_t msb = 0;
        std::int32_t lsb = 0;
        bool indexable = false;
        std::vector<graph::BitRef> explicitBits; ///< for register clock pins (C): one bit
    };
    std::vector<Object> match(const std::vector<Named>& items, ObjectKind kind, std::string_view pattern,
                              bool hierarchical, bool nocase) const;
    void buildNames();      ///< ports, nets and instance pins (on first use)
    void buildRegisters();  ///< "<name>_reg" entries per register (on first register-pin query)

    const graph::DesignView& view_;
    bool namesBuilt_ = false;
    bool registersBuilt_ = false;
    std::vector<Named> ports_;
    std::vector<Named> nets_;
    std::vector<Named> pins_;
    std::vector<Named> regs_;      ///< per register: name "<prefix><net>_reg", bits = Q
    std::vector<graph::BitRef> regClock_; ///< clock bit of regs_[i]
    std::vector<CellInfo> regInfo_;       ///< parallel to regs_
    bool instancesBuilt_ = false;
    std::vector<std::string> insts_;      ///< hierarchical instance names (tops excluded)
    std::vector<CellInfo> instInfo_;      ///< parallel to insts_
    std::set<std::string, std::less<>> attrNames_; ///< upper-case attribute names on registers/instances
    bool loadsBuilt_ = false;
    bool quartusBuilt_ = false;
    std::vector<Named> qregs_;  ///< regs_ with Quartus names ("u|sig", "u|sig[3]"), same order
    std::vector<Named> qpins_;  ///< pins_ with Quartus names, same order
    std::vector<std::string> qinsts_; ///< insts_ with Quartus names
    std::vector<Named> qregsFull_;    ///< qregs_ with "entity:instance" levels ("sync2:u_s|s1_reg")
    std::vector<Named> qpinsFull_;
    std::vector<std::string> qinstsFull_;
    void buildQuartus();
    /// driver of a bit -> flip-flop load pins on it: (register, pin name, Q bit of that flip-flop)
    std::map<std::tuple<int, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>,
             std::vector<std::tuple<std::size_t, std::string, graph::BitRef>>>
        loads_;
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> regsByNet_; ///< (inst, net) -> regs_ indices
};

/// Tcl glob match (* ? and backslash escape).
[[nodiscard]] bool globMatch(std::string_view pattern, std::string_view text, bool nocase = false);

struct ClockDef {
    std::string name;
    std::vector<graph::BitRef> sources;  ///< empty = virtual clock
    std::vector<std::string> sourceNames;
    std::optional<double> periodNs;
    std::vector<double> waveform;
    bool add = false;
    // create_generated_clock
    bool generated = false;
    std::vector<graph::BitRef> masterSource;
    std::string masterClock;
    int divideBy = 1;
    int multiplyBy = 1;
    bool invert = false;
    SourceLocation loc;
};

enum class GroupKind : std::uint8_t { Asynchronous, LogicallyExclusive, PhysicallyExclusive };

struct ClockGroups {
    std::string name;
    GroupKind kind = GroupKind::Asynchronous;
    std::vector<std::vector<std::string>> groups; ///< clock name patterns, resolved after clock analysis
    /// per group (parallel to `groups`, may be shorter): the clocks on these
    /// nets, pins or ports belong to the group (get_clocks -of_objects)
    std::vector<std::vector<graph::BitRef>> memberBits;
    SourceLocation loc;
};

/// set_false_path / set_max_delay -datapath_only between clocks
/// (-from [get_clocks a] -to [get_clocks b]): the crossings from a to b are
/// handled as asynchronous by the designer.
struct ClockException {
    std::string command;                  ///< "set_false_path" / "set_max_delay -datapath_only"
    std::vector<std::string> from;        ///< clock name patterns
    std::vector<std::string> to;
    std::vector<graph::BitRef> fromBits;  ///< get_clocks -of_objects
    std::vector<graph::BitRef> toBits;
    SourceLocation loc;
};

/// Which pins of a destination register an exception names.
enum class EndpointRole : std::uint8_t { Any, Data, AsyncReset };

struct PathEndpoint {
    graph::BitRef bit; ///< register Q bit (or port)
    EndpointRole role = EndpointRole::Any;
};

/// set_false_path / set_max_delay -datapath_only between registers, pins,
/// ports or clocks, at least one side being registers, pins or ports
/// (-from [get_cells a_reg] -to [get_cells b_reg], -from <clock> -to
/// <registers>, -to <reset pins> alone): the designer has constrained these
/// crossings; their clock relation counts as asynchronous. An empty side
/// (no -from or no -to) matches everything.
struct PathException {
    std::string command;
    std::vector<graph::BitRef> from;  ///< register Q bits or input ports
    std::vector<PathEndpoint> to;     ///< register Q bits with the pins named
    std::vector<std::string> fromClocks; ///< clock name patterns
    std::vector<std::string> toClocks;
    std::vector<graph::BitRef> fromClockBits; ///< get_clocks -of_objects
    std::vector<graph::BitRef> toClockBits;
    SourceLocation loc;
};

/// set_input_delay -clock <clk> <ports>: assigns input ports to a clock
/// domain for CDC analysis (delay values are not used).
struct InputDelay {
    std::string clock;                 ///< clock name (resolved after clock analysis)
    std::vector<graph::BitRef> clockBits; ///< -clock [get_clocks -of_objects ...]: the clock on these bits
    std::vector<graph::BitRef> bits;
    std::vector<std::string> portNames;
    bool clockFall = false;
    SourceLocation loc;
};

/// set_output_delay -clock <clk> <ports>: the clock the receiver of these
/// output ports samples them with (delay values are not used). The drivers
/// of the ports must be synchronous to it.
using OutputDelay = InputDelay;

/// The subset of SDC/XDC that clock and CDC analysis use.
struct ConstraintSet {
    std::vector<ClockDef> clocks;
    std::vector<ClockGroups> groups;
    std::vector<InputDelay> inputDelays;
    std::vector<OutputDelay> outputDelays;
    std::vector<ClockException> clockExceptions;
    std::vector<PathException> pathExceptions;
    std::map<std::string, int> ignoredCommands; ///< command -> occurrences (reported, not applied)
    std::vector<std::filesystem::path> files;
};

struct SdcOptions {
    /// Directories searched for relative `source` paths after the directory
    /// of the file that runs `source` (e.g. the directory Quartus or Vivado
    /// runs the scripts from).
    std::vector<std::filesystem::path> sourceDirs;
};

/// Reads the constraint files of one analysis into one ConstraintSet. All
/// files share one Tcl interpreter, as in Vivado and Quartus: variables and
/// procedures defined in one file are visible in the files read after it.
/// Diagnostics: CON-PARSE, CON-OPEN, CON-UNKNOWN-OPTION, CON-UNSUPPORTED-OPTION,
/// CON-UNSUPPORTED-COMMAND, CON-UNKNOWN-COMMAND, CON-UNMATCHED, CON-IGNORED,
/// CON-DUPLICATE-CLOCK.
class SdcReader {
public:
    SdcReader(IObjectResolver& resolver, ConstraintSet& out, DiagnosticSink& sink, SdcOptions options = {});
    SdcReader(const SdcReader&) = delete;
    SdcReader& operator=(const SdcReader&) = delete;
    ~SdcReader();

    /// Interprets SDC/XDC text (a Tcl subset) named `fileName` (a path when
    /// it runs `source` with relative paths).
    void parse(std::string_view text, const std::string& fileName);
    /// Reads and interprets a file. Returns false if it cannot be read.
    bool readFile(const std::filesystem::path& file);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// One file with its own interpreter (see SdcReader).
void parseSdc(std::string_view text, const std::string& fileName, IObjectResolver& resolver, ConstraintSet& out,
              DiagnosticSink& sink);

/// Reads one file with its own interpreter (see SdcReader). Returns false if
/// the file cannot be read.
bool readSdcFile(const std::filesystem::path& file, IObjectResolver& resolver, ConstraintSet& out,
                 DiagnosticSink& sink);

} // namespace opencdc::constraints
