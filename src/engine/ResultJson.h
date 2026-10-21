#pragma once

#include "engine/AnalysisSession.h"

#include <iosfwd>
#include <string>

namespace opencdc::engine {

inline constexpr const char* kResultFormatName = "opencdc-result";
inline constexpr int kResultFormatVersion = 2; ///< 2: stage-5 CDC fields (structures, reset paths, ...)

/// Serializes a result as JSON (UTF-8). `indent` < 0 gives compact output.
[[nodiscard]] std::string toJson(const AnalysisResult& result, int indent = 2);

/// Same text as toJson(), streamed: the register and path lists are written
/// element by element, so memory does not grow with the output size.
void writeJson(const AnalysisResult& result, std::ostream& os, int indent = 2);

} // namespace opencdc::engine
