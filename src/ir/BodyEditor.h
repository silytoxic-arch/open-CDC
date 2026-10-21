#pragma once
// Language-neutral construction helpers for one ModuleBody, shared by the
// HDL frontends (Verilog via slang, VHDL): cell creation with constant
// folding and structural hashing, extraction of synchronous reset / enable
// from a register's next-state logic, and the final cleanup passes (dead
// storage, dead cells, multiple drivers).

#include "core/Diagnostic.h"
#include "core/SourceLocation.h"
#include "ir/Design.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace opencdc::ir {

class BodyEditor {
public:
    /// `processWord` names procedural blocks in diagnostics ("always block",
    /// "process").
    explicit BodyEditor(ModuleBody& body, std::string processWord = "always block")
        : body_(body), processWord_(std::move(processWord)) {}

    /// Adds a cell, unless it folds to a constant / existing signal or an
    /// identical cell exists (structural hashing). Returns its output.
    SigSpec emit(Cell c);
    SigSpec cell(CellKind kind, std::vector<SigSpec> inputs, std::uint32_t width, const SourceLocation& origin,
                 bool isSigned = false);
    /// 1-bit truth value (reduce-or of wider signals).
    SigSpec toBool(const SigSpec& s, const SourceLocation& origin);
    /// Adds a cell without folding or hashing (opaque cells: each is distinct).
    SigSpec addUnique(Cell c);
    /// The cell driving temp net `net`, if any.
    [[nodiscard]] const Cell* driverOf(NetId net) const;

    /// Moves a synchronous reset (outermost "if (srst) q <= CONST") and a
    /// clock enable ("if (en) q <= d", other branch holds q) out of r.d.
    void extractControls(Register& r, const SourceLocation& origin);

    /// Adds a latch; `unintended` latches that survive cleanup are warned about.
    void addLatch(Latch l, bool unintended);

    /// Final passes, in order: removes registers/latches whose outputs are
    /// never read (IR-DEAD-STORAGE info, IR-LATCH-INFERRED warnings for the
    /// surviving unintended latches), removes dead cells (nets are renumbered:
    /// `onRemap(old -> new)` lets the caller update its own net maps), and
    /// reports overlapping drivers (IR-MULTIDRIVER).
    void finish(DiagnosticSink& sink, const std::function<void(const std::vector<std::uint32_t>&)>& onRemap);

private:
    void pruneDeadStorage(DiagnosticSink& sink);
    void removeDeadCells(const std::function<void(const std::vector<std::uint32_t>&)>& onRemap);
    void checkMultipleDrivers(DiagnosticSink& sink);

    ModuleBody& body_;
    std::string processWord_;
    std::unordered_map<std::uint32_t, std::size_t> driverCell_; ///< temp net -> cell index
    std::unordered_map<std::string, SigSpec> cellCache_;        ///< structural hashing
    std::vector<bool> latchUnintended_;                         ///< parallel to body_.latches
};

} // namespace opencdc::ir
