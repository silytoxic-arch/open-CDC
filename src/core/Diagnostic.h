#pragma once

#include "core/SourceLocation.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace opencdc {

enum class Severity { Info, Warning, Error, Fatal };

[[nodiscard]] std::string_view toString(Severity severity) noexcept;

/// A message produced by any analysis stage. `code` is a stable identifier
/// such as "PRJ-PARSE" or "FE-SLANG-UnknownModule" that tests, filters and
/// future waivers can rely on; `message` is human readable.
struct Diagnostic {
    std::string code;
    Severity severity = Severity::Error;
    std::string message;
    SourceLocation location;
    std::vector<std::string> notes;

    [[nodiscard]] bool isError() const noexcept {
        return severity == Severity::Error || severity == Severity::Fatal;
    }
};

/// "file:line:col: error[CODE]: message" (location omitted when unknown).
[[nodiscard]] std::string format(const Diagnostic& diag);

/// Receives diagnostics from analysis stages.
class DiagnosticSink {
public:
    DiagnosticSink() = default;
    DiagnosticSink(const DiagnosticSink&) = delete;
    DiagnosticSink& operator=(const DiagnosticSink&) = delete;
    virtual ~DiagnosticSink() = default;

    virtual void report(Diagnostic diag) = 0;
};

/// Stores every diagnostic in order of arrival.
class CollectingDiagnosticSink final : public DiagnosticSink {
public:
    void report(Diagnostic diag) override;

    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const noexcept { return diags_; }
    [[nodiscard]] std::vector<Diagnostic> take() noexcept;
    [[nodiscard]] std::size_t count(Severity severity) const noexcept;
    [[nodiscard]] bool hasErrors() const noexcept;

private:
    std::vector<Diagnostic> diags_;
};

} // namespace opencdc
