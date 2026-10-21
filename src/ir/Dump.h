#pragma once

#include "ir/Design.h"

#include <string>

namespace opencdc::ir {

/// Verilog-like text for a signal, e.g. "{a[3:0], 2'b01, b}".
[[nodiscard]] std::string formatSig(const ModuleBody& body, const SigSpec& sig);

/// Deterministic, human-readable dump of one module body.
[[nodiscard]] std::string dumpBody(const ModuleBody& body);

/// Dump of all bodies, the instance tree and unsupported items. Stable across
/// runs, used for golden-file regression tests and `opencdc --dump-ir`.
[[nodiscard]] std::string dumpDesign(const Design& design);

} // namespace opencdc::ir
