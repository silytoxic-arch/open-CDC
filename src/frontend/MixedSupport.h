#pragma once
// Internal interface between the language frontends for mixed-language
// designs (not part of the public frontend API).

#include "frontend/Frontend.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace opencdc::frontend::detail {

struct VerilogMixedOptions {
    /// Definitions provided by the other language: instances of them are kept
    /// as unresolved children (parameters and port actuals as written).
    std::set<std::string> foreignModules;
    /// Top modules; empty: inferred by slang.
    std::vector<std::string> tops;
    /// Output: non-local parameter names of every module definition.
    std::map<std::string, std::vector<std::string>>* moduleParams = nullptr;
    /// Built-in vendor primitive models: connections to ports they do not
    /// model are reported as information (FE-PRIMITIVE-PORT), not errors.
    std::set<std::string> lenientModules;
};

/// The slang frontend with mixed-language options.
[[nodiscard]] Result<FrontendOutput> elaborateVerilog(const FrontendInput& input, DiagnosticSink& sink,
                                                      const VerilogMixedOptions* mixed);

} // namespace opencdc::frontend::detail
