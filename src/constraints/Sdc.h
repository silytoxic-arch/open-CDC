#pragma once

#include "core/Diagnostic.h"
#include "core/SourceLocation.h"
#include "graph/DesignView.h"

#include <filesystem>
#include <map>
#include <unordered_map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opencdc::constraints {

enum class ObjectKind : std::uint8_t { Port, Pin, Net, Clock };

/// A design object returned by get_ports / get_pins / get_nets / get_clocks.
struct Object {
    ObjectKind kind = ObjectKind::Net;
    std::string name;                ///< matched name (for clocks: the name pattern)
    std::vector<graph::BitRef> bits; ///< resolved bits (empty for clocks)
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
};

/// Resolver over a flattened design (Vivado-style names: instance levels
/// joined with '/', top module name omitted; register pins "<reg>_reg/Q" and
/// "<reg>_reg/C" are mapped to the register output and clock).
class DesignObjectResolver final : public IObjectResolver {
public:
    explicit DesignObjectResolver(const graph::DesignView& view);

    std::vector<Object> ports(std::string_view pattern, bool nocase) override;
    std::vector<Object> pins(std::string_view pattern, bool hierarchical, bool nocase) override;
    std::vector<Object> nets(std::string_view pattern, bool hierarchical, bool nocase) override;

private:
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
    SourceLocation loc;
};

/// set_input_delay -clock <clk> <ports>: assigns input ports to a clock
/// domain for CDC analysis (delay values are not used).
struct InputDelay {
    std::string clock;                 ///< clock name (resolved after clock analysis)
    std::vector<graph::BitRef> bits;
    std::vector<std::string> portNames;
    bool clockFall = false;
    SourceLocation loc;
};

/// The subset of SDC/XDC that clock and CDC analysis use.
struct ConstraintSet {
    std::vector<ClockDef> clocks;
    std::vector<ClockGroups> groups;
    std::vector<InputDelay> inputDelays;
    std::map<std::string, int> ignoredCommands; ///< command -> occurrences (reported, not applied)
    std::vector<std::filesystem::path> files;
};

/// Interprets SDC/XDC text (a Tcl subset) and appends to `out`.
/// Diagnostics: CON-PARSE, CON-UNKNOWN-OPTION, CON-UNSUPPORTED-OPTION,
/// CON-UNMATCHED, CON-IGNORED, CON-DUPLICATE-CLOCK.
void parseSdc(std::string_view text, const std::string& fileName, IObjectResolver& resolver, ConstraintSet& out,
              DiagnosticSink& sink);

/// Reads a file and calls parseSdc. Returns false if the file cannot be read.
bool readSdcFile(const std::filesystem::path& file, IObjectResolver& resolver, ConstraintSet& out,
                 DiagnosticSink& sink);

} // namespace opencdc::constraints
